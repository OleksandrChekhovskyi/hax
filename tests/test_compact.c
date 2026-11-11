/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "agent_stream.h"
#include "compact.h"
#include "config.h"
#include "harness.h"
#include "provider.h"
#include "session.h"
#include "xalloc.h"
#include "transport/api_error.h"
#include "transport/http.h"

static void test_over_threshold(void)
{
    EXPECT(!compact_over_threshold(100, 0, 85));
    EXPECT(!compact_over_threshold(100, -1, 85));
    EXPECT(!compact_over_threshold(-1, 1000, 85));
    EXPECT(!compact_over_threshold(100, 1000, 0));
    EXPECT(!compact_over_threshold(100, 1000, 101));

    /* A large window leaves the reserve free at the percentage, so the percentage decides. */
    EXPECT(!compact_over_threshold(849999, 1000000, 85));
    EXPECT(compact_over_threshold(850000, 1000000, 85));
    EXPECT(!compact_over_threshold(500, 1001, 50)); /* 500.5 rounds up */
    EXPECT(compact_over_threshold(501, 1001, 50));

    /* Otherwise the reserve does: 16384 tokens, or a quarter of a small window. */
    EXPECT(!compact_over_threshold(983615, 1000000, 100));
    EXPECT(compact_over_threshold(983616, 1000000, 100));
    EXPECT(!compact_over_threshold(7499, 10000, 85));
    EXPECT(compact_over_threshold(7500, 10000, 85));

    EXPECT(!compact_over_threshold(LONG_MAX - 16385, LONG_MAX, 100));
    EXPECT(compact_over_threshold(LONG_MAX - 16384, LONG_MAX, 100));
}

static void add_window_usage(struct agent_session *session, long input_tokens)
{
    struct stream_usage usage = {
        .input_tokens = input_tokens,
        .output_tokens = 50,
        .cached_tokens = -1,
        .cache_write_tokens = -1,
        .cache_write_1h_tokens = -1,
        .cost = -1,
    };
    agent_session_add_turn_usage(session, NULL, &usage, 1000, NULL, ITEM_ORIGIN_NONE);
}

static void test_due(void)
{
    /* Drive the config tiers via runtime overrides so the test is
     * hermetic regardless of env / file config. */
    config_set_override("compact.auto", "1");
    config_set_override("compact.threshold", "90");
    config_set_override("context_limit", "1000000");
    struct provider provider = {.name = "test"};
    struct agent_session session = {0};
    session.model = xstrdup("model");
    agent_session_add_user(&session, "hello");

    EXPECT(!compact_due(&session, &provider, NULL)); /* nothing reported yet */
    add_window_usage(&session, 899949);              /* 89.99% < 90% */
    EXPECT(!compact_due(&session, &provider, NULL));
    add_window_usage(&session, 899950); /* exactly 90% */
    EXPECT(compact_due(&session, &provider, NULL));
    EXPECT(!compact_due(&session, NULL, NULL));

    /* The live limit decides, so a larger window settles the debt without compacting. */
    config_set_override("context_limit", "2000000");
    EXPECT(!compact_due(&session, &provider, NULL));
    config_set_override("context_limit", "1000000");

    /* Disabled via config: never auto-compacts, even when far over. */
    config_set_override("compact.auto", "0");
    add_window_usage(&session, 10000000);
    EXPECT(!compact_due(&session, &provider, NULL));

    /* Out-of-range threshold falls back to the 85% default. */
    config_set_override("compact.auto", "1");
    config_set_override("compact.threshold", "0");
    add_window_usage(&session, 849949);
    EXPECT(!compact_due(&session, &provider, NULL));
    add_window_usage(&session, 849950);
    EXPECT(compact_due(&session, &provider, NULL));

    /* Automatic compaction stops asking after repeated failures. */
    session.compact_failures = COMPACT_MAX_AUTO_FAILURES;
    EXPECT(!compact_due(&session, &provider, NULL));

    agent_session_free(&session);
    config_set_override("context_limit", NULL);
    config_set_override("compact.threshold", NULL);
    config_set_override("compact.auto", NULL);
}

/* The prompt about to be sent and items the last usage record did not measure both count. */
static void test_due_counts_unsent_input(void)
{
    config_set_override("compact.auto", "1");
    config_set_override("compact.threshold", "90");
    config_set_override("context_limit", "1000000");
    struct provider provider = {.name = "test"};
    struct agent_session session = {0};
    session.model = xstrdup("model");
    agent_session_add_user(&session, "hello");
    add_window_usage(&session, 899940); /* 10 tokens short */

    char prompt[41];
    memset(prompt, 'p', 40);
    prompt[40] = '\0';
    EXPECT(!compact_due(&session, &provider, prompt + 4));
    EXPECT(compact_due(&session, &provider, prompt));

    agent_session_append(&session, (struct item){.kind = ITEM_USER_MESSAGE,
                                                 .text = xstrdup(prompt),
                                                 .origin = ITEM_ORIGIN_TASK_NOTE});
    EXPECT(compact_due(&session, &provider, NULL));

    agent_session_free(&session);
    config_set_override("context_limit", NULL);
    config_set_override("compact.threshold", NULL);
    config_set_override("compact.auto", NULL);
}

enum mock_script {
    MOCK_SUCCESS,
    MOCK_RETRY_ONCE,
    MOCK_ALWAYS_TOOL_CALL,
    MOCK_ERROR,
    MOCK_ERROR_WITHOUT_MESSAGE,
    MOCK_NO_SUMMARY,
    MOCK_INCOMPLETE,
    MOCK_STREAM_RETRY,
    MOCK_STREAM_RETRY_INCOMPLETE,
};

struct mock_state {
    enum mock_script script;
    /* Requests estimated above this many tokens, or with tools when `reject_tools` is set, fail
     * as too large with `reject_kind`. */
    long reject_above;
    int reject_tools;
    enum api_error_kind reject_kind;
    int stream_calls;
    long request_tokens[8];
    /* What poll and checkpoint report; the later fields take over once the stream reaches them. */
    enum agent_signal signal;
    enum agent_signal signal_after_text;
    enum agent_signal signal_at_end;
    size_t last_request_items;
    size_t last_request_tools;
    int last_request_is_text;
    int last_request_was_shortened;
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

static void emit_rejection(stream_cb cb, void *user, enum api_error_kind kind)
{
    struct stream_event ev = {.kind = EV_ERROR,
                              .u.error = {.message = "request too large", .kind = kind}};
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
    EXPECT(ctx != NULL);
    long tokens =
        text_token_estimate(ctx->system_prompt) + items_token_estimate(ctx->items, ctx->n_items);
    if ((size_t)mock.stream_calls < sizeof(mock.request_tokens) / sizeof(mock.request_tokens[0]))
        mock.request_tokens[mock.stream_calls] = tokens;
    mock.last_request_items = ctx->n_items;
    mock.last_request_tools = ctx->n_tools;
    mock.last_request_is_text = ctx->n_items == 1 && ctx->items[0].text &&
                                strstr(ctx->items[0].text, "<conversation>") != NULL;
    mock.last_request_was_shortened =
        mock.last_request_is_text &&
        strstr(strstr(ctx->items[0].text, "<conversation>"), " omitted") != NULL;
    mock.stream_calls++;

    if ((mock.reject_above && tokens > mock.reject_above) || (mock.reject_tools && ctx->n_tools)) {
        emit_rejection(cb, user, mock.reject_kind);
        return 0;
    }

    if ((mock.script == MOCK_RETRY_ONCE && mock.stream_calls == 1) ||
        mock.script == MOCK_ALWAYS_TOOL_CALL) {
        emit_text(cb, user, "I'll inspect the files");
        emit_tool_call(cb, user);
        emit_done(cb, user, 100);
        return 0;
    }
    if (mock.script == MOCK_ERROR_WITHOUT_MESSAGE) {
        emit_error(cb, user, NULL, NULL);
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
    if (tick && tick(tick_user))
        return 0;
    if (mock.script != MOCK_NO_SUMMARY)
        emit_text(cb, user, "## Goal\n- continue");
    if (mock.signal_after_text)
        mock.signal = mock.signal_after_text;
    if (mock.script == MOCK_INCOMPLETE || (tick && tick(tick_user)))
        return 0;
    emit_done(cb, user, mock.stream_calls == 1 ? 100 : 200);
    if (mock.signal_at_end)
        mock.signal = mock.signal_at_end;
    return 0;
}

static enum agent_signal mock_signal(void *user)
{
    (void)user;
    return mock.signal;
}

static void init_session(struct agent_session *session)
{
    memset(session, 0, sizeof(*session));
    session->model = xstrdup("model");
    session->provider_id = "test";
    agent_session_append(session,
                         (struct item){.kind = ITEM_USER_MESSAGE, .text = xstrdup("old history")});
}

static struct compact_result run_compaction_as(struct agent_session *session,
                                               struct provider *provider,
                                               struct session_log *session_log,
                                               enum compact_reason reason)
{
    struct compact_params params = {
        .session = session,
        .provider = provider,
        .session_log = session_log,
        .reason = reason,
        .hooks =
            {
                .poll = mock_signal,
                .checkpoint = mock_signal,
            },
    };
    struct compact_result result;
    compact_run(&params, &result);
    return result;
}

static struct compact_result run_compaction(struct agent_session *session,
                                            struct provider *provider,
                                            struct session_log *session_log)
{
    return run_compaction_as(session, provider, session_log, COMPACT_MANUAL);
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
    /* The summarized history stays; the seed and its usage record are appended after it. */
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
    /* The dead attempt was a served request of its own: its usage record belongs to the
     * summarized prefix, and only the accepted attempt's record follows the seed. */
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(session.n_items == 5);
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[1].usage->usage.input_tokens == 50);
    EXPECT(session.items[3].origin == ITEM_ORIGIN_COMPACT_SEED);
    EXPECT(session.items[4].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[4].usage->usage.input_tokens == 100);

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
    /* No terminal event ever arrived, but the banked usage is still recorded. */
    EXPECT(result.outcome == COMPACT_NO_SUMMARY);
    EXPECT(session.n_items == 2);
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[1].origin == ITEM_ORIGIN_COMPACTION);
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
    /* The first seed and its usage record plus the prompt — the summarized prefix is not
     * re-read. */
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

    /* The rejected attempt's usage record belongs to the summarized prefix; only the accepted
     * attempt's record follows the seed. */
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
    /* The window and prompt, a re-ask after the rejected call, then the window as text. */
    EXPECT(result.attempts == 3);
    EXPECT(mock.stream_calls == 3);
    EXPECT(mock.last_request_is_text);
    EXPECT(session.n_items == 4);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    for (size_t i = 1; i < session.n_items; i++)
        EXPECT(session.items[i].kind == ITEM_TURN_USAGE);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

/* A window with a system prompt, one tool, and `user_turns` prompts that each read
 * `output_bytes`. */
static void init_large_session(struct agent_session *session, size_t user_turns,
                               size_t output_bytes)
{
    init_session(session);
    session->system_prompt = xstrdup("system");
    session->tools = xcalloc(1, sizeof(*session->tools));
    session->tools[0].name = "read";
    session->n_tools = 1;
    char *output = xmalloc(output_bytes + 1);
    memset(output, 'x', output_bytes);
    output[output_bytes] = '\0';
    for (size_t i = 0; i < user_turns; i++) {
        agent_session_add_user(session, "read the next file");
        agent_session_append(session, (struct item){.kind = ITEM_TOOL_CALL,
                                                    .call_id = xstrdup("c"),
                                                    .tool_name = xstrdup("read"),
                                                    .tool_arguments_json = xstrdup("{}")});
        agent_session_append(session, (struct item){.kind = ITEM_TOOL_RESULT,
                                                    .call_id = xstrdup("c"),
                                                    .output = xstrdup(output)});
    }
    free(output);
}

/* The text requests after the full-window one, each smaller than the last. */
static void expect_shrinking_requests(int count)
{
    for (int i = 2; i < count; i++)
        EXPECT(mock.request_tokens[i] < mock.request_tokens[i - 1]);
}

/* A window rejected as too long is summarized as text without tools, shortened until the
 * provider accepts it. */
static void test_overflow_summarizes_shortened_text(void)
{
    struct agent_session session;
    init_large_session(&session, 3, 8000);
    size_t history_items = session.n_items;
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_SUCCESS);
    mock.reject_above = 3000;
    mock.reject_kind = API_ERROR_CONTEXT_OVERFLOW;

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(result.attempts >= 3);
    expect_shrinking_requests(result.attempts);
    EXPECT(mock.last_request_is_text);
    EXPECT(mock.last_request_tools == 0);
    EXPECT(mock.last_request_was_shortened);

    EXPECT(session.items[history_items + 1].origin == ITEM_ORIGIN_COMPACT_SEED);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

/* The whole text goes first, then each shrink step must send a smaller request, and four steps
 * end the run. */
static void test_overflow_attempts_are_bounded(void)
{
    struct agent_session session;
    init_large_session(&session, 3, 8000);
    size_t history_items = session.n_items;
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_SUCCESS);
    mock.reject_above = 1;
    mock.reject_kind = API_ERROR_CONTEXT_OVERFLOW;

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_PROVIDER_ERROR);
    EXPECT_STR_EQ(result.error_message, "request too large");
    EXPECT(result.attempts == 6);
    expect_shrinking_requests(result.attempts);
    EXPECT(session.n_items == history_items);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

/* Text carries no media, so a byte-size rejection first retries the window whole as text. */
static void test_payload_too_large_sends_whole_text(void)
{
    struct agent_session session;
    init_large_session(&session, 1, 400);
    struct item *result_item = &session.items[session.n_items - 1];
    result_item->images = xcalloc(1, sizeof(*result_item->images));
    result_item->images[0].mime = xstrdup("image/png");
    result_item->images[0].data_b64 = xstrdup("AAAA");
    result_item->n_images = 1;
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_SUCCESS);
    mock.reject_above = ITEM_IMAGE_TOKEN_ESTIMATE;
    mock.reject_kind = API_ERROR_PAYLOAD_TOO_LARGE;

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(result.attempts == 2);
    EXPECT(mock.last_request_is_text);
    EXPECT(!mock.last_request_was_shortened);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

/* Tool schemas can overflow a short history; the text request, which has none, still goes out. */
static void test_overflow_from_tools_sends_text(void)
{
    struct agent_session session;
    init_large_session(&session, 0, 0);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_SUCCESS);
    mock.reject_tools = 1;
    mock.reject_kind = API_ERROR_CONTEXT_OVERFLOW;

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(result.attempts == 2);
    EXPECT(mock.last_request_is_text);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

/* With a known window, even the first text request aims at a share of it less the reserve. */
static void test_text_fits_known_window(void)
{
    config_set_override("context_limit", "4000");
    struct agent_session session;
    init_large_session(&session, 3, 8000);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_SUCCESS);
    mock.reject_above = 4000;
    mock.reject_kind = API_ERROR_PAYLOAD_TOO_LARGE;

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(result.attempts == 2);
    EXPECT(mock.request_tokens[0] > 4000);
    EXPECT(mock.request_tokens[1] <= 2550); /* 85% of 4000 less a quarter */
    EXPECT(mock.last_request_was_shortened);

    compact_result_destroy(&result);
    agent_session_free(&session);
    config_set_override("context_limit", NULL);
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

static void expect_auto_failure(struct agent_session *session, struct provider *provider,
                                enum mock_script script, int failures, int suspended)
{
    reset_mock(script);
    struct compact_result result = run_compaction_as(session, provider, NULL, COMPACT_THRESHOLD);
    EXPECT(result.outcome != COMPACT_COMPLETE);
    EXPECT(session->compact_failures == failures);
    EXPECT(result.auto_suspended == suspended);
    compact_result_destroy(&result);
}

/* Automatic failures count toward the breaker, while manual and cancelled ones do not, and any
 * success clears it. */
static void test_auto_failures_trip_breaker(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};

    expect_auto_failure(&session, &provider, MOCK_ERROR, 1, 0);
    reset_mock(MOCK_ERROR);
    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(session.compact_failures == 1);
    compact_result_destroy(&result);
    reset_mock(MOCK_SUCCESS);
    mock.signal_at_end = AGENT_SIGNAL_ABORT;
    result = run_compaction_as(&session, &provider, NULL, COMPACT_THRESHOLD);
    EXPECT(result.outcome == COMPACT_CANCELLED);
    EXPECT(session.compact_failures == 1);
    compact_result_destroy(&result);

    expect_auto_failure(&session, &provider, MOCK_NO_SUMMARY, 2, 0);
    expect_auto_failure(&session, &provider, MOCK_ERROR, 3, 1);
    expect_auto_failure(&session, &provider, MOCK_ERROR, 4, 0);

    reset_mock(MOCK_SUCCESS);
    result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(session.compact_failures == 0);
    compact_result_destroy(&result);

    agent_session_free(&session);
}

/* A failure is a provider error whether or not the provider explained it. */
static void test_failure_without_message(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_ERROR_WITHOUT_MESSAGE);

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_PROVIDER_ERROR);
    EXPECT(result.error_message == NULL);
    EXPECT(session.n_items == 1);

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
     * a usage record without replacing usable history. */
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
    mock.signal_at_end = AGENT_SIGNAL_ABORT;

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_CANCELLED);
    /* A late abort wins over a complete summary: old history survives and
     * the completed attempt still receives its usage record. */
    EXPECT(session.n_items == 2);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void test_pause_before_summary_cancels(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_SUCCESS);
    mock.signal = AGENT_SIGNAL_PAUSE;

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_CANCELLED);
    EXPECT(session.n_items == 1);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void test_pause_after_summary_starts_keeps_it(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_SUCCESS);
    mock.signal_after_text = AGENT_SIGNAL_PAUSE;

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(session.items[2].origin == ITEM_ORIGIN_COMPACT_SEED);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void test_abort_mid_summary_cancels(void)
{
    struct agent_session session;
    init_session(&session);
    struct provider provider = {.name = "test", .stream = mock_stream};
    reset_mock(MOCK_SUCCESS);
    mock.signal_after_text = AGENT_SIGNAL_ABORT;

    struct compact_result result = run_compaction(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_CANCELLED);
    EXPECT(session.n_items == 1);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

int main(void)
{
    /* Outside any repository, so recorded sessions neither run git nor depend on the checkout. */
    EXPECT(chdir(t_tempdir()) == 0);
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    test_over_threshold();
    test_due();
    test_due_counts_unsent_input();
    test_applies_summary();
    test_retried_attempt_usage_is_billed();
    test_cancelled_retry_usage_is_billed();
    test_compacts_twice();
    test_keeps_one_session();
    test_tool_call_retries_are_bounded();
    test_overflow_summarizes_shortened_text();
    test_overflow_attempts_are_bounded();
    test_payload_too_large_sends_whole_text();
    test_overflow_from_tools_sends_text();
    test_text_fits_known_window();
    test_resumed_session_floors_at_seed();
    test_preserves_failure();
    test_failure_without_message();
    test_auto_failures_trip_breaker();
    test_rejects_empty_summary();
    test_rejects_incomplete_response();
    test_discards_cancelled_summary();
    test_pause_before_summary_cancels();
    test_pause_after_summary_starts_keeps_it();
    test_abort_mid_summary_cancels();
    T_REPORT();
}
