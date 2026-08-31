/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "compact.h"
#include "config.h"
#include "harness.h"
#include "provider.h"
#include "session.h"
#include "xalloc.h"
#include "transport/http.h"

static void test_over_threshold(void)
{
    EXPECT(!compact_over_threshold(100, 0, 85));
    EXPECT(!compact_over_threshold(100, -1, 85));
    EXPECT(!compact_over_threshold(-1, 1000, 85));
    EXPECT(!compact_over_threshold(100, 1000, 0));
    EXPECT(!compact_over_threshold(100, 1000, 101));

    EXPECT(!compact_over_threshold(8499, 10000, 85));
    EXPECT(compact_over_threshold(8500, 10000, 85));
    EXPECT(compact_over_threshold(9999, 10000, 85));
    EXPECT(!compact_over_threshold(9999, 10000, 100));
    EXPECT(compact_over_threshold(10000, 10000, 100));

    EXPECT(!compact_over_threshold(LONG_MAX - 1, LONG_MAX, 100));
    EXPECT(compact_over_threshold(LONG_MAX, LONG_MAX, 100));
}

static void test_should_auto(void)
{
    /* Drive the config tiers via runtime overrides so the test is
     * hermetic regardless of env / file config. */
    config_set_override("compact.auto", "1");
    config_set_override("compact.threshold", "90");

    EXPECT(!compact_should_auto(8999, 10000)); /* 89.99% < 90% */
    EXPECT(compact_should_auto(9000, 10000));  /* exactly 90% */
    EXPECT(!compact_should_auto(9999, 0));     /* unknown window */

    /* Disabled via config: never auto-compacts, even when far over. */
    config_set_override("compact.auto", "0");
    EXPECT(!compact_should_auto(100000, 10000));

    /* Out-of-range threshold falls back to the 85% default. */
    config_set_override("compact.auto", "1");
    config_set_override("compact.threshold", "0");
    EXPECT(!compact_should_auto(8499, 10000));
    EXPECT(compact_should_auto(8500, 10000));
}

enum mock_script {
    MOCK_SUCCESS,
    MOCK_RETRY_ONCE,
    MOCK_ALWAYS_TOOL_CALL,
    MOCK_ERROR,
    MOCK_NO_SUMMARY,
    MOCK_INCOMPLETE,
    MOCK_STREAM_RETRY,
    MOCK_STREAM_RETRY_INCOMPLETE,
};

struct mock_state {
    enum mock_script script;
    int stream_calls;
    int cancel_requested;
    int observed_events;
    size_t last_request_items;
};

static struct mock_state mock;

static void reset_mock(enum mock_script script)
{
    memset(&mock, 0, sizeof(mock));
    mock.script = script;
}

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

static void emit_done(stream_cb cb, void *user, long input_tokens)
{
    struct stream_event ev = {.kind = EV_DONE, .u.done = {.usage = usage_tokens(input_tokens, 10)}};
    cb(&ev, user);
}

static void emit_error(stream_cb cb, void *user, const char *message,
                       const struct stream_usage *usage)
{
    struct stream_event ev = {.kind = EV_ERROR, .u.error = {.message = message, .usage = usage}};
    cb(&ev, user);
}

static void emit_retry(stream_cb cb, void *user, const struct stream_usage *usage)
{
    struct stream_event ev = {.kind = EV_RETRY,
                              .u.retry = {.attempt = 1, .max_attempts = 5, .usage = usage}};
    cb(&ev, user);
}

static void emit_tool_call(stream_cb cb, void *user)
{
    struct stream_event start = {.kind = EV_TOOL_CALL_START,
                                 .u.tool_call_start = {.id = "c1", .name = "read"}};
    struct stream_event delta = {.kind = EV_TOOL_CALL_DELTA,
                                 .u.tool_call_delta = {.id = "c1", .args_delta = "{}"}};
    struct stream_event end = {.kind = EV_TOOL_CALL_END, .u.tool_call_end = {.id = "c1"}};
    cb(&start, user);
    cb(&delta, user);
    cb(&end, user);
}

static int mock_stream(struct provider *provider, const struct context *ctx, const char *model,
                       stream_cb cb, void *user, http_tick_cb tick, void *tick_user)
{
    (void)provider;
    (void)model;
    (void)tick;
    (void)tick_user;
    EXPECT(ctx != NULL);
    mock.last_request_items = ctx->n_items;
    mock.stream_calls++;

    if ((mock.script == MOCK_RETRY_ONCE && mock.stream_calls == 1) ||
        mock.script == MOCK_ALWAYS_TOOL_CALL) {
        emit_text(cb, user, "I'll inspect the files");
        emit_tool_call(cb, user);
        emit_done(cb, user, 100);
        return 0;
    }
    if (mock.script == MOCK_ERROR) {
        struct stream_usage usage = usage_tokens(150, 5);
        emit_text(cb, user, "partial");
        emit_error(cb, user, "summary failed", &usage);
        return 0;
    }
    if (mock.script == MOCK_STREAM_RETRY || mock.script == MOCK_STREAM_RETRY_INCOMPLETE) {
        struct stream_usage retry_usage = usage_tokens(50, -1);
        emit_retry(cb, user, &retry_usage);
        /* A pause cancelling the redo leaves the banked usage with no terminal event. */
        if (mock.script == MOCK_STREAM_RETRY_INCOMPLETE)
            return 0;
    }
    if (mock.script != MOCK_NO_SUMMARY)
        emit_text(cb, user, "## Goal\n- continue");
    if (mock.script == MOCK_INCOMPLETE)
        return 0;
    emit_done(cb, user, mock.stream_calls == 1 ? 100 : 200);
    return 0;
}

static int observe_event(const struct stream_event *event, void *user)
{
    (void)event;
    (void)user;
    mock.observed_events++;
    return 0;
}

static int is_cancelled(void *user)
{
    (void)user;
    return mock.cancel_requested;
}

static void init_session(struct agent_session *session)
{
    memset(session, 0, sizeof(*session));
    session->model = xstrdup("model");
    session->provider_id = "test";
    agent_session_append(session,
                         (struct item){.kind = ITEM_USER_MESSAGE, .text = xstrdup("old history")});
}

static struct compact_result run_compaction(struct agent_session *session,
                                            struct provider *provider,
                                            struct session_log *session_log)
{
    struct compact_params params = {
        .session = session,
        .provider = provider,
        .session_log = session_log,
        .hooks =
            {
                .on_event = observe_event,
                .is_cancelled = is_cancelled,
            },
    };
    struct compact_result result;
    compact_run(&params, &result);
    return result;
}

static void test_applies_summary(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_SUCCESS);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(result.attempts == 1);
    EXPECT(mock.observed_events == 2);
    /* The summarized history stays; the seed and its usage footer are appended after it. */
    EXPECT(session.n_items == 4);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    EXPECT(session.items[1].kind == ITEM_TURN_BOUNDARY);
    EXPECT(session.items[2].kind == ITEM_USER_MESSAGE &&
           session.items[2].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(session.items[2].text && strstr(session.items[2].text, "earlier part") != NULL);
    EXPECT(session.items[2].text && strstr(session.items[2].text, "continue") != NULL);
    EXPECT(session.items[3].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[3].usage->usage.input_tokens == 100);

    /* What the model sees starts at the seed. */
    struct context window = agent_session_context(&session);
    EXPECT(window.n_items == 2);
    EXPECT(window.items[0].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(window.items[1].kind == ITEM_TURN_USAGE);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void test_retried_attempt_usage_is_billed(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_STREAM_RETRY);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    /* The dead attempt's banked usage folds into the accepted attempt's footer, so the
     * persisted record matches what the live accounting hooks billed. */
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(session.n_items == 4);
    EXPECT(session.items[3].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[3].usage->usage.input_tokens == 150);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void test_cancelled_retry_usage_is_billed(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_STREAM_RETRY_INCOMPLETE);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    /* No terminal event ever arrived, but the banked usage still gets a footer. */
    EXPECT(result.outcome == COMPACT_NO_SUMMARY);
    EXPECT(session.n_items == 2);
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[1].usage->usage.input_tokens == 50);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

/* A second pass summarizes only the window the first one left. */
static void test_compacts_twice(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_SUCCESS);

    struct compact_result first = run_compaction(&session, &provider, NULL);
    EXPECT(first.outcome == COMPACT_COMPLETE);
    /* One history item plus the summarization prompt. */
    EXPECT(mock.last_request_items == 2);
    compact_result_destroy(&first);

    struct compact_result second = run_compaction(&session, &provider, NULL);
    EXPECT(second.outcome == COMPACT_COMPLETE);
    /* The first seed and its footer plus the prompt — the summarized prefix is not re-read. */
    EXPECT(mock.last_request_items == 3);
    EXPECT(session.n_items == 7);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    EXPECT(session.items[2].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(session.items[5].origin == ITEM_ORIGIN_COMPACT_SEED);

    /* The newest seed wins; the first one is now part of the summarized prefix. */
    struct context window = agent_session_context(&session);
    EXPECT(window.n_items == 2);
    EXPECT(window.items == &session.items[5]);

    /* Cutting past a seed, as /undo does, falls back to the older one and then to no seed. */
    EXPECT(items_context_floor(session.items, 5) == 2);
    EXPECT(items_context_floor(session.items, 2) == 0);

    compact_result_destroy(&second);
    agent_session_free(&session);
}

static void free_items(struct item *items, size_t n_items)
{
    for (size_t i = 0; i < n_items; i++)
        item_free(&items[i]);
    free(items);
}

/* Compaction continues the session file it was given, so one compacted conversation stays one
 * entry in the picker instead of a chain of them. */
static void test_keeps_one_session(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    struct session_log *session_log = session_log_open("test", "model", NULL, NULL, NULL);
    EXPECT(session_log != NULL);
    session_log_append(session_log, session.items, session.n_items);
    char *path = xstrdup(session_log_path(session_log));
    reset_mock(MOCK_RETRY_ONCE);

    struct compact_result result = run_compaction(&session, &provider, session_log);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(result.attempts == 2);
    EXPECT_STR_EQ(path, session_log_path(session_log));

    /* The rejected attempt's footer belongs to the summarized prefix; only the accepted
     * attempt's footer follows the seed. */
    EXPECT(session.n_items == 5);
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[1].usage->usage.input_tokens == 100);
    EXPECT(session.items[3].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(session.items[4].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[4].usage->usage.input_tokens == 200);

    struct item *recorded = NULL;
    size_t n_recorded = 0;
    EXPECT(session_load(path, &recorded, &n_recorded, NULL) == 0);
    EXPECT(n_recorded == 5);
    EXPECT_STR_EQ(recorded[0].text, "old history");
    EXPECT(recorded[3].kind == ITEM_USER_MESSAGE && recorded[3].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(recorded[4].usage->usage.input_tokens == 200);

    free_items(recorded, n_recorded);
    free(path);
    compact_result_destroy(&result);
    session_log_close(session_log);
    agent_session_free(&session);
}

static void test_tool_call_retries_are_bounded(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_ALWAYS_TOOL_CALL);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_NO_SUMMARY);
    EXPECT(result.attempts == 4);
    EXPECT(mock.stream_calls == 4);
    EXPECT(mock.last_request_items == 11);
    EXPECT(session.n_items == 5);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    for (size_t i = 1; i < session.n_items; i++)
        EXPECT(session.items[i].kind == ITEM_TURN_USAGE);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

/* The floor is derived from the recorded seed, so a resumed file needs no carried-over state. */
static void test_resumed_session_floors_at_seed(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    struct session_log *session_log = session_log_open("test", "model", NULL, NULL, NULL);
    EXPECT(session_log != NULL);
    session_log_append(session_log, session.items, session.n_items);
    char *path = xstrdup(session_log_path(session_log));
    reset_mock(MOCK_SUCCESS);

    struct compact_result result = run_compaction(&session, &provider, session_log);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    compact_result_destroy(&result);
    session_log_close(session_log);
    agent_session_free(&session);

    struct agent_session resumed;
    memset(&resumed, 0, sizeof(resumed));
    EXPECT(session_load(path, &resumed.items, &resumed.n_items, NULL) == 0);
    resumed.cap_items = resumed.n_items;
    EXPECT(resumed.n_items == 4);
    EXPECT_STR_EQ(resumed.items[0].text, "old history");

    struct context window = agent_session_context(&resumed);
    EXPECT(window.n_items == 2);
    EXPECT(window.items[0].origin == ITEM_ORIGIN_COMPACT_SEED);

    free(path);
    agent_session_free(&resumed);
}

static void test_preserves_failure(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_ERROR);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_PROVIDER_ERROR);
    EXPECT_STR_EQ(result.error_message, "summary failed");
    /* Partial summary text never enters history; billed failure usage does. */
    EXPECT(session.n_items == 2);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[1].usage->usage.input_tokens == 150);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void test_rejects_empty_summary(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_NO_SUMMARY);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_NO_SUMMARY);
    /* A clean but empty response is still a completed, billed attempt; it gets
     * a footer without replacing usable history. */
    EXPECT(session.n_items == 2);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void test_rejects_incomplete_response(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_INCOMPLETE);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_NO_SUMMARY);
    EXPECT(session.n_items == 1);
    EXPECT_STR_EQ(session.items[0].text, "old history");

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void test_discards_cancelled_summary(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_SUCCESS);
    mock.cancel_requested = 1;

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_CANCELLED);
    /* A late cancel wins over a complete summary: old history survives and
     * the completed attempt still receives its footer. */
    EXPECT(session.n_items == 2);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

int main(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    unsetenv("HAX_NO_SESSION");
    test_over_threshold();
    test_should_auto();
    test_applies_summary();
    test_retried_attempt_usage_is_billed();
    test_cancelled_retry_usage_is_billed();
    test_compacts_twice();
    test_keeps_one_session();
    test_tool_call_retries_are_bounded();
    test_resumed_session_floors_at_seed();
    test_preserves_failure();
    test_rejects_empty_summary();
    test_rejects_incomplete_response();
    test_discards_cancelled_summary();
    T_REPORT();
}
