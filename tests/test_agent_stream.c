/* SPDX-License-Identifier: MIT */
#include <stddef.h>

#include "agent_stream.h"
#include "harness.h"
#include "provider.h"
#include "turn.h"
#include "transport/http.h"

static struct stream_usage usage_tokens(long input, long output)
{
    return (struct stream_usage){
        .input_tokens = input,
        .output_tokens = output,
        .cached_tokens = -1,
        .cache_write_tokens = -1,
        .cache_write_1h_tokens = -1,
        .cost = -1,
    };
}

static void emit_text(stream_cb cb, void *user, const char *text)
{
    struct stream_event ev = {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = text}};
    cb(&ev, user);
}

static void emit_done(stream_cb cb, void *user, struct stream_usage usage)
{
    struct stream_event ev = {.kind = EV_DONE, .u.done = {.usage = usage}};
    cb(&ev, user);
}

enum script {
    SCRIPT_RETRY_THEN_COMPLETE,
    SCRIPT_ERROR,
    /* Tick before and after the first text, stopping without a terminal event when told to. */
    SCRIPT_TICKED,
};

static enum script script;
static int observed_events;
static int ticks_cut_before_content;

static int scripted_stream(struct provider *p, const struct context *ctx, const char *model,
                           stream_cb cb, void *user, http_tick_cb tick, void *tick_user)
{
    (void)p;
    (void)ctx;
    (void)model;

    if (script == SCRIPT_RETRY_THEN_COMPLETE) {
        struct stream_usage retry_usage = usage_tokens(40, -1);
        struct stream_event retry = {
            .kind = EV_RETRY, .u.retry = {.attempt = 1, .max_attempts = 5, .usage = &retry_usage}};
        emit_text(cb, user, "doomed attempt");
        cb(&retry, user);
        emit_text(cb, user, "answer");
        emit_done(cb, user, usage_tokens(100, 20));
        return 0;
    }
    if (script == SCRIPT_ERROR) {
        struct stream_usage usage = usage_tokens(10, 0);
        struct stream_event error = {.kind = EV_ERROR,
                                     .u.error = {.message = "overloaded", .usage = &usage}};
        cb(&error, user);
        return 0;
    }

    EXPECT(tick != NULL);
    if (tick(tick_user)) {
        ticks_cut_before_content++;
        return -1;
    }
    emit_text(cb, user, "answer");
    if (tick(tick_user))
        return -1;
    emit_done(cb, user, usage_tokens(100, 20));
    return 0;
}

static int observe(const struct stream_event *ev, void *user)
{
    (void)ev;
    (void)user;
    observed_events++;
    return 0;
}

static enum agent_signal requested_signal;

static enum agent_signal poll_requested(void *user)
{
    (void)user;
    return requested_signal;
}

static void request(struct agent_stream *stream, struct provider *provider)
{
    struct context ctx = {0};
    agent_stream_request(stream, provider, &ctx, "model", observe, NULL, poll_requested, NULL);
}

static void test_signal_stops(void)
{
    struct turn assembly;
    turn_init(&assembly);
    EXPECT(!agent_signal_stops(AGENT_SIGNAL_NONE, &assembly));
    EXPECT(agent_signal_stops(AGENT_SIGNAL_PAUSE, &assembly));
    EXPECT(agent_signal_stops(AGENT_SIGNAL_ABORT, &assembly));

    struct stream_event text = {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = "partial"}};
    turn_consume(&assembly, &text);
    EXPECT(!agent_signal_stops(AGENT_SIGNAL_PAUSE, &assembly));
    EXPECT(agent_signal_stops(AGENT_SIGNAL_ABORT, &assembly));
    turn_reset(&assembly);
}

/* The observer sees every event, but capture does not depend on it: the terminal attempt alone
 * measures the window, and every served attempt is logged. */
static void test_collects_attempts_and_terminal_usage(void)
{
    struct provider provider = {.name = "test", .stream = scripted_stream};
    struct agent_stream stream;
    agent_stream_init(&stream);
    script = SCRIPT_RETRY_THEN_COMPLETE;
    requested_signal = AGENT_SIGNAL_NONE;
    observed_events = 0;

    request(&stream, &provider);
    EXPECT(stream.assembly.state == TURN_DONE);
    EXPECT(stream.assembly.n_items == 1);
    EXPECT_STR_EQ(stream.assembly.items[0].text, "answer");
    EXPECT(stream.usage.input_tokens == 100 && stream.usage.output_tokens == 20);
    EXPECT(stream.attempts.count == 2);
    EXPECT(stream.attempts.attempts[0].usage.input_tokens == 40);
    EXPECT(stream.attempts.attempts[1].usage.input_tokens == 100);
    EXPECT(agent_stream_has_usage(&stream));
    EXPECT(observed_events == 4);

    agent_stream_destroy(&stream);
}

/* A later request restarts the assembly, terminal usage, and error but keeps earlier attempts. */
static void test_requests_accumulate(void)
{
    struct provider provider = {.name = "test", .stream = scripted_stream};
    struct agent_stream stream;
    agent_stream_init(&stream);
    requested_signal = AGENT_SIGNAL_NONE;

    script = SCRIPT_ERROR;
    request(&stream, &provider);
    EXPECT(stream.assembly.state == TURN_FAILED);
    EXPECT_STR_EQ(stream.error_message, "overloaded");

    script = SCRIPT_TICKED;
    request(&stream, &provider);
    EXPECT(stream.assembly.state == TURN_DONE);
    EXPECT(stream.usage.input_tokens == 100);
    EXPECT(stream.attempts.count == 2);
    EXPECT(stream.error_message == NULL);

    script = SCRIPT_ERROR;
    request(&stream, &provider);
    EXPECT_STR_EQ(stream.error_message, "overloaded");

    agent_stream_destroy(&stream);
}

static void test_poll_cuts_by_signal(void)
{
    struct provider provider = {.name = "test", .stream = scripted_stream};
    struct agent_stream stream;
    agent_stream_init(&stream);
    script = SCRIPT_TICKED;
    ticks_cut_before_content = 0;

    /* A pause cuts a request that has streamed nothing. */
    requested_signal = AGENT_SIGNAL_PAUSE;
    request(&stream, &provider);
    EXPECT(ticks_cut_before_content == 1);
    EXPECT(agent_stream_cut_by(&stream, AGENT_SIGNAL_PAUSE));
    EXPECT(!agent_stream_cut_by(&stream, AGENT_SIGNAL_NONE));

    /* An abort cuts at any point. */
    requested_signal = AGENT_SIGNAL_ABORT;
    request(&stream, &provider);
    EXPECT(ticks_cut_before_content == 2);
    EXPECT(agent_stream_cut_by(&stream, AGENT_SIGNAL_ABORT));

    agent_stream_destroy(&stream);
}

static int pause_after_content_ticks;

static enum agent_signal pause_after_first_tick(void *user)
{
    (void)user;
    return pause_after_content_ticks++ > 0 ? AGENT_SIGNAL_PAUSE : AGENT_SIGNAL_NONE;
}

/* Once content has streamed, a pause lets the request finish. */
static void test_pause_after_content_finishes(void)
{
    struct provider provider = {.name = "test", .stream = scripted_stream};
    struct agent_stream stream;
    agent_stream_init(&stream);
    script = SCRIPT_TICKED;
    pause_after_content_ticks = 0;

    struct context ctx = {0};
    agent_stream_request(&stream, &provider, &ctx, "model", NULL, NULL, pause_after_first_tick,
                         NULL);
    EXPECT(pause_after_content_ticks == 2);
    EXPECT(stream.assembly.state == TURN_DONE);
    EXPECT(!agent_stream_cut_by(&stream, AGENT_SIGNAL_PAUSE));

    agent_stream_destroy(&stream);
}

int main(void)
{
    test_signal_stops();
    test_collects_attempts_and_terminal_usage();
    test_requests_accumulate();
    test_poll_cuts_by_signal();
    test_pause_after_content_finishes();
    T_REPORT();
}
