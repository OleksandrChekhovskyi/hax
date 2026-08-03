/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "provider.h"
#include "providers/openai_events.h"

#define MAX_CAPTURED_EVENTS 16

struct captured_event {
    enum stream_event_kind kind;
    char *text;
    char *id;
    char *name;
    char *args_delta;
    char *message;
    int http_status;
    struct stream_usage usage;
    long progress_processed;
    long progress_total;
    long progress_cached;
};

struct capture_state {
    struct captured_event events[MAX_CAPTURED_EVENTS];
    size_t n_events;
};

static int capture_event(const struct stream_event *event, void *user)
{
    struct capture_state *capture = user;
    if (capture->n_events >= MAX_CAPTURED_EVENTS) {
        FAIL("%s", "too many events captured");
        return 0;
    }
    struct captured_event *captured = &capture->events[capture->n_events++];
    memset(captured, 0, sizeof(*captured));
    captured->kind = event->kind;
    switch (event->kind) {
    case EV_TEXT_DELTA:
        captured->text = strdup(event->u.text_delta.text);
        break;
    case EV_TOOL_CALL_START:
        captured->id = strdup(event->u.tool_call_start.id);
        captured->name = strdup(event->u.tool_call_start.name);
        break;
    case EV_TOOL_CALL_DELTA:
        captured->id = strdup(event->u.tool_call_delta.id);
        captured->args_delta = strdup(event->u.tool_call_delta.args_delta);
        break;
    case EV_TOOL_CALL_END:
        captured->id = strdup(event->u.tool_call_end.id);
        break;
    case EV_REASONING_ITEM:
        break;
    case EV_REASONING_DELTA:
        captured->text = strdup(event->u.reasoning_delta.text ? event->u.reasoning_delta.text : "");
        break;
    case EV_RETRY:
        break;
    case EV_PROGRESS:
        captured->progress_processed = event->u.progress.processed;
        captured->progress_total = event->u.progress.total;
        captured->progress_cached = event->u.progress.cache;
        break;
    case EV_DONE:
        captured->message = strdup(event->u.done.stop_reason ? event->u.done.stop_reason : "");
        captured->usage = event->u.done.usage;
        break;
    case EV_ERROR:
        captured->message = strdup(event->u.error.message ? event->u.error.message : "");
        captured->http_status = event->u.error.http_status;
        if (event->u.error.usage)
            captured->usage = *event->u.error.usage;
        else
            captured->usage = (struct stream_usage){-1, -1, -1, -1, -1, -1};
        break;
    }
    return 0;
}

static void reset_capture(struct capture_state *capture)
{
    for (size_t i = 0; i < capture->n_events; i++) {
        free(capture->events[i].text);
        free(capture->events[i].id);
        free(capture->events[i].name);
        free(capture->events[i].args_delta);
        free(capture->events[i].message);
    }
    memset(capture, 0, sizeof(*capture));
}

/* NOLINTBEGIN(bugprone-macro-parentheses): the arguments name declared variables */
#define EVENTS_FIXTURE(capture, parser)                                                            \
    struct capture_state capture = {0};                                                            \
    struct openai_events parser;                                                                   \
    openai_events_init(&parser, capture_event, &capture)

#define EVENTS_FIXTURE_FREE(capture, parser)                                                       \
    do {                                                                                           \
        openai_events_free(&parser);                                                               \
        reset_capture(&capture);                                                                   \
    } while (0)
/* NOLINTEND(bugprone-macro-parentheses) */

static void feed_content(struct openai_events *parser, const char *text)
{
    char data[512];
    snprintf(data, sizeof(data), "{\"choices\":[{\"delta\":{\"content\":\"%s\"}}]}", text);
    openai_events_feed(parser, data);
}

static void feed_finish(struct openai_events *parser, const char *reason)
{
    char data[512];
    snprintf(data, sizeof(data), "{\"choices\":[{\"delta\":{},\"finish_reason\":\"%s\"}]}", reason);
    openai_events_feed(parser, data);
}

static void test_text_delta(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_content(&parser, "Hello");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_TEXT_DELTA);
    EXPECT_STR_EQ(capture.events[0].text, "Hello");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_empty_content_ignored(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"content\":\"\"}}]}");
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"content\":null}}]}");
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}");
    EXPECT(capture.n_events == 0);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_reasoning_delta_openrouter(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning\":\"Hmm\"}}]}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_REASONING_DELTA);
    EXPECT_STR_EQ(capture.events[0].text, "Hmm");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_reasoning_delta_llamacpp(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning_content\":\"Let\"}}]}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_REASONING_DELTA);
    EXPECT_STR_EQ(capture.events[0].text, "Let");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_reasoning_then_content(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning\":\"Think\"}}]}");
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning\":\"ing\"}}]}");
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"content\":\"Answer\"}}]}");
    EXPECT(capture.n_events == 3);
    EXPECT(capture.events[0].kind == EV_REASONING_DELTA);
    EXPECT(capture.events[1].kind == EV_REASONING_DELTA);
    EXPECT(capture.events[2].kind == EV_TEXT_DELTA);
    EXPECT_STR_EQ(capture.events[2].text, "Answer");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_empty_reasoning_ignored(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning\":\"\"}}]}");
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning\":null}}]}");
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning_content\":\"\"}}]}");
    EXPECT(capture.n_events == 0);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_tool_call_lifecycle(void)
{
    EVENTS_FIXTURE(capture, parser);

    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                                "\"index\":0,\"id\":\"call_1\",\"type\":\"function\","
                                "\"function\":{\"name\":\"bash\",\"arguments\":\"\"}}]}}]}");
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                                "\"index\":0,\"function\":{\"arguments\":\"{\\\"cmd\\\":\"}}]}}]}");
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                                "\"index\":0,\"function\":{\"arguments\":\"\\\"ls\\\"}\"}}]}}]}");
    feed_finish(&parser, "tool_calls");

    openai_events_feed(&parser, "[DONE]");

    EXPECT(capture.n_events == 5);
    EXPECT(capture.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(capture.events[0].id, "call_1");
    EXPECT_STR_EQ(capture.events[0].name, "bash");
    EXPECT(capture.events[1].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(capture.events[1].id, "call_1");
    EXPECT_STR_EQ(capture.events[1].args_delta, "{\"cmd\":");
    EXPECT(capture.events[2].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(capture.events[2].args_delta, "\"ls\"}");
    EXPECT(capture.events[3].kind == EV_TOOL_CALL_END);
    EXPECT_STR_EQ(capture.events[3].id, "call_1");
    EXPECT(capture.events[4].kind == EV_DONE);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_tool_call_id_and_name_across_deltas(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                                "\"index\":0,\"id\":\"c1\"}]}}]}");
    EXPECT(capture.n_events == 0);
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                                "\"index\":0,\"function\":{\"name\":\"bash\"}}]}}]}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(capture.events[0].id, "c1");
    EXPECT_STR_EQ(capture.events[0].name, "bash");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_tool_call_args_before_metadata_buffered(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                                "\"index\":0,\"id\":\"c1\","
                                "\"function\":{\"arguments\":\"{\\\"cmd\\\":\"}}]}}]}");
    EXPECT(capture.n_events == 0);
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                                "\"index\":0,\"function\":{\"name\":\"bash\","
                                "\"arguments\":\"\\\"ls\\\"}\"}}]}}]}");
    EXPECT(capture.n_events == 3);
    EXPECT(capture.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(capture.events[0].id, "c1");
    EXPECT_STR_EQ(capture.events[0].name, "bash");
    EXPECT(capture.events[1].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(capture.events[1].args_delta, "{\"cmd\":");
    EXPECT(capture.events[2].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(capture.events[2].args_delta, "\"ls\"}");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_parallel_tool_calls(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":["
                                "{\"index\":0,\"id\":\"a\",\"function\":{\"name\":\"x\"}},"
                                "{\"index\":1,\"id\":\"b\",\"function\":{\"name\":\"y\"}}"
                                "]}}]}");
    feed_finish(&parser, "tool_calls");
    openai_events_feed(&parser, "[DONE]");

    EXPECT(capture.n_events == 5);
    EXPECT(capture.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(capture.events[0].id, "a");
    EXPECT(capture.events[1].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(capture.events[1].id, "b");
    EXPECT(capture.events[2].kind == EV_TOOL_CALL_END);
    EXPECT(capture.events[3].kind == EV_TOOL_CALL_END);

    EXPECT_STR_EQ(capture.events[2].id, "a");
    EXPECT_STR_EQ(capture.events[3].id, "b");
    EXPECT(capture.events[4].kind == EV_DONE);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_tool_call_without_id_synthesizes(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                                "\"index\":0,\"function\":{\"name\":\"bash\","
                                "\"arguments\":\"{}\"}}]}}]}");
    feed_finish(&parser, "tool_calls");
    openai_events_feed(&parser, "[DONE]");

    EXPECT(capture.n_events == 4);
    EXPECT(capture.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(capture.events[0].id, "call_0");
    EXPECT_STR_EQ(capture.events[0].name, "bash");
    EXPECT(capture.events[1].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(capture.events[1].id, "call_0");
    EXPECT_STR_EQ(capture.events[1].args_delta, "{}");
    EXPECT(capture.events[2].kind == EV_TOOL_CALL_END);
    EXPECT_STR_EQ(capture.events[2].id, "call_0");
    EXPECT(capture.events[3].kind == EV_DONE);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_tool_call_delta_without_index_defaults_to_zero(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                                "\"id\":\"x\",\"function\":{\"name\":\"y\"}}]}}]}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(capture.events[0].id, "x");
    EXPECT_STR_EQ(capture.events[0].name, "y");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_finish_reason_stop_defers_done_until_sentinel(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "stop");
    EXPECT(capture.n_events == 0);
    EXPECT(parser.terminal_emitted == 0);
    openai_events_feed(&parser, "[DONE]");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_DONE);
    EXPECT_STR_EQ(capture.events[0].message, "stop");
    EXPECT(parser.terminal_emitted == 1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_finish_reason_tool_calls_emits_done(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "tool_calls");
    openai_events_feed(&parser, "[DONE]");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_DONE);
    EXPECT_STR_EQ(capture.events[0].message, "tool_calls");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_finish_reason_length_emits_error(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "length");
    EXPECT(capture.n_events == 0);
    EXPECT(parser.terminal_emitted == 0);
    openai_events_feed(&parser, "{\"choices\":[],\"usage\":{"
                                "\"prompt_tokens\":1234,\"completion_tokens\":56}}");
    openai_events_feed(&parser, "[DONE]");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_ERROR);
    EXPECT(strstr(capture.events[0].message, "length") != NULL);
    EXPECT(capture.events[0].usage.input_tokens == 1234);
    EXPECT(capture.events[0].usage.output_tokens == 56);
    EXPECT(parser.terminal_emitted == 1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_finish_reason_length_error_on_close_without_sentinel(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "length");
    EXPECT(capture.n_events == 0);
    openai_events_finalize(&parser);
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_ERROR);
    EXPECT(strstr(capture.events[0].message, "length") != NULL);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_finish_reason_content_filter_emits_error(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "content_filter");
    EXPECT(capture.n_events == 0);
    openai_events_feed(&parser, "[DONE]");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_ERROR);
    EXPECT(strstr(capture.events[0].message, "content_filter") != NULL);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_done_sentinel(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "[DONE]");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_DONE);
    EXPECT(parser.terminal_emitted == 1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_double_termination_gated(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "stop");
    feed_finish(&parser, "stop");
    openai_events_feed(&parser, "[DONE]");
    openai_events_feed(&parser, "[DONE]");
    EXPECT(capture.n_events == 1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_events_after_terminal_ignored(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "[DONE]");
    feed_content(&parser, "late");
    openai_events_feed(&parser, "{\"error\":{\"message\":\"late\"}}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_DONE);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_error_object_emits_error(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "{\"error\":{\"message\":\"Rate limit exceeded\",\"code\":429}}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_ERROR);
    EXPECT_STR_EQ(capture.events[0].message, "Rate limit exceeded");
    EXPECT(parser.terminal_emitted == 1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_unparseable_json_ignored(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "not json");
    openai_events_feed(&parser, "");
    openai_events_feed(&parser, NULL);
    EXPECT(capture.n_events == 0);
    EXPECT(parser.terminal_emitted == 0);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_missing_choices_ignored(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "{\"foo\":\"bar\"}");
    openai_events_feed(&parser, "{\"choices\":[]}");
    EXPECT(capture.n_events == 0);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_finalize_without_terminal_emits_error(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_content(&parser, "hi");
    openai_events_finalize(&parser);
    EXPECT(capture.n_events == 2);
    EXPECT(capture.events[0].kind == EV_TEXT_DELTA);
    EXPECT(capture.events[1].kind == EV_ERROR);
    EXPECT(strstr(capture.events[1].message, "stream ended") != NULL);
    EXPECT(parser.terminal_emitted == 1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_finalize_after_done_no_extra_event(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "stop");
    openai_events_feed(&parser, "[DONE]");
    openai_events_finalize(&parser);
    EXPECT(capture.n_events == 1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_finalize_after_finish_without_sentinel_emits_done(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "stop");
    openai_events_finalize(&parser);
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_DONE);
    EXPECT_STR_EQ(capture.events[0].message, "stop");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_usage_default_unknown(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "stop");
    openai_events_feed(&parser, "[DONE]");
    EXPECT(capture.events[0].kind == EV_DONE);
    EXPECT(capture.events[0].usage.input_tokens == -1);
    EXPECT(capture.events[0].usage.output_tokens == -1);
    EXPECT(capture.events[0].usage.cached_tokens == -1);
    EXPECT(capture.events[0].usage.cost < 0);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_usage_captured_from_trailing_chunk(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "stop");
    openai_events_feed(&parser, "{\"choices\":[],\"usage\":{"
                                "\"prompt_tokens\":1234,\"completion_tokens\":56,"
                                "\"prompt_tokens_details\":{\"cached_tokens\":1000}}}");
    openai_events_feed(&parser, "[DONE]");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_DONE);
    EXPECT(capture.events[0].usage.input_tokens == 1234);
    EXPECT(capture.events[0].usage.output_tokens == 56);
    EXPECT(capture.events[0].usage.cached_tokens == 1000);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_usage_without_cached_details(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "stop");
    openai_events_feed(&parser, "{\"choices\":[],\"usage\":{"
                                "\"prompt_tokens\":10,\"completion_tokens\":20}}");
    openai_events_feed(&parser, "[DONE]");
    EXPECT(capture.events[0].usage.input_tokens == 10);
    EXPECT(capture.events[0].usage.output_tokens == 20);
    EXPECT(capture.events[0].usage.cached_tokens == -1);
    EXPECT(capture.events[0].usage.cost < 0);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_usage_cost_captured(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "stop");
    openai_events_feed(&parser, "{\"choices\":[],\"usage\":{"
                                "\"prompt_tokens\":10,\"completion_tokens\":20,"
                                "\"cost\":0.0123}}");
    openai_events_feed(&parser, "[DONE]");
    EXPECT(capture.events[0].kind == EV_DONE);
    EXPECT(capture.events[0].usage.cost > 0.0122 && capture.events[0].usage.cost < 0.0124);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_usage_cache_write_captured(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "stop");
    openai_events_feed(&parser, "{\"choices\":[],\"usage\":{"
                                "\"prompt_tokens\":2810,\"completion_tokens\":4,"
                                "\"cost\":0.01059525,"
                                "\"prompt_tokens_details\":{\"cached_tokens\":0,"
                                "\"cache_write_tokens\":2807}}}");
    openai_events_feed(&parser, "[DONE]");
    EXPECT(capture.events[0].usage.input_tokens == 2810);
    EXPECT(capture.events[0].usage.cached_tokens == 0);
    EXPECT(capture.events[0].usage.cache_write_tokens == 2807);

    EXPECT(capture.events[0].usage.cache_write_1h_tokens == -1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_usage_cache_write_1h_attributed_from_request(void)
{
    EVENTS_FIXTURE(capture, parser);
    parser.cache_write_1h = 1;
    feed_finish(&parser, "stop");
    openai_events_feed(&parser, "{\"choices\":[],\"usage\":{"
                                "\"prompt_tokens\":2816,\"completion_tokens\":4,"
                                "\"prompt_tokens_details\":{\"cache_write_tokens\":2813}}}");
    openai_events_feed(&parser, "[DONE]");
    EXPECT(capture.events[0].usage.cache_write_tokens == 2813);
    EXPECT(capture.events[0].usage.cache_write_1h_tokens == 2813);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_usage_cache_write_1h_needs_a_write(void)
{
    EVENTS_FIXTURE(capture, parser);
    parser.cache_write_1h = 1;
    feed_finish(&parser, "stop");
    openai_events_feed(&parser, "{\"choices\":[],\"usage\":{"
                                "\"prompt_tokens\":2810,\"completion_tokens\":4,"
                                "\"prompt_tokens_details\":{\"cached_tokens\":2807}}}");
    openai_events_feed(&parser, "[DONE]");
    EXPECT(capture.events[0].usage.cached_tokens == 2807);
    EXPECT(capture.events[0].usage.cache_write_tokens == -1);
    EXPECT(capture.events[0].usage.cache_write_1h_tokens == -1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_progress_ignored_when_flag_off(void)
{
    EVENTS_FIXTURE(capture, parser);
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":{}}],"
                                "\"prompt_progress\":{\"total\":100,\"cache\":0,"
                                "\"processed\":50,\"time_ms\":42}}");
    EXPECT(capture.n_events == 0);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_progress_emitted_when_flag_on(void)
{
    EVENTS_FIXTURE(capture, parser);
    parser.emit_progress = 1;
    openai_events_feed(&parser, "{\"choices\":[{\"delta\":"
                                "{\"role\":\"assistant\",\"content\":null}}],"
                                "\"prompt_progress\":{\"total\":1000,\"cache\":200,"
                                "\"processed\":600,\"time_ms\":123}}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_PROGRESS);
    EXPECT(capture.events[0].progress_total == 1000);
    EXPECT(capture.events[0].progress_cached == 200);
    EXPECT(capture.events[0].progress_processed == 600);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_progress_missing_fields_default_zero(void)
{
    EVENTS_FIXTURE(capture, parser);
    parser.emit_progress = 1;
    openai_events_feed(&parser, "{\"choices\":[],"
                                "\"prompt_progress\":{\"processed\":50}}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_PROGRESS);
    EXPECT(capture.events[0].progress_processed == 50);
    EXPECT(capture.events[0].progress_total == 0);
    EXPECT(capture.events[0].progress_cached == 0);
    EVENTS_FIXTURE_FREE(capture, parser);
}

int main(void)
{
    test_text_delta();
    test_empty_content_ignored();
    test_reasoning_delta_openrouter();
    test_reasoning_delta_llamacpp();
    test_reasoning_then_content();
    test_empty_reasoning_ignored();
    test_tool_call_lifecycle();
    test_tool_call_id_and_name_across_deltas();
    test_tool_call_args_before_metadata_buffered();
    test_parallel_tool_calls();
    test_tool_call_without_id_synthesizes();
    test_tool_call_delta_without_index_defaults_to_zero();
    test_finish_reason_stop_defers_done_until_sentinel();
    test_finish_reason_tool_calls_emits_done();
    test_finish_reason_length_emits_error();
    test_finish_reason_length_error_on_close_without_sentinel();
    test_finish_reason_content_filter_emits_error();
    test_done_sentinel();
    test_double_termination_gated();
    test_events_after_terminal_ignored();
    test_error_object_emits_error();
    test_unparseable_json_ignored();
    test_missing_choices_ignored();
    test_finalize_without_terminal_emits_error();
    test_finalize_after_done_no_extra_event();
    test_finalize_after_finish_without_sentinel_emits_done();
    test_usage_default_unknown();
    test_usage_captured_from_trailing_chunk();
    test_usage_without_cached_details();
    test_usage_cost_captured();
    test_usage_cache_write_captured();
    test_usage_cache_write_1h_attributed_from_request();
    test_usage_cache_write_1h_needs_a_write();
    test_progress_ignored_when_flag_off();
    test_progress_emitted_when_flag_on();
    test_progress_missing_fields_default_zero();
    T_REPORT();
}
