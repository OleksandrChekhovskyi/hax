/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "provider.h"
#include "providers/chat_events.h"

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
    char *response_id;
    char *served_model;
    char *route;
    long progress_processed;
    long progress_total;
    long progress_cached;
};

static char *dup_or_null(const char *value)
{
    return value ? strdup(value) : NULL;
}

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
        captured->text = strdup(event->u.reasoning_item.json);
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
        captured->response_id = dup_or_null(event->u.done.response.id);
        captured->served_model = dup_or_null(event->u.done.response.model);
        captured->route = dup_or_null(event->u.done.response.route);
        break;
    case EV_ERROR:
        captured->message = strdup(event->u.error.message ? event->u.error.message : "");
        captured->http_status = event->u.error.http_status;
        if (event->u.error.usage)
            captured->usage = *event->u.error.usage;
        else
            captured->usage = (struct stream_usage){-1, -1, -1, -1, -1, -1};
        if (event->u.error.response) {
            captured->response_id = dup_or_null(event->u.error.response->id);
            captured->served_model = dup_or_null(event->u.error.response->model);
            captured->route = dup_or_null(event->u.error.response->route);
        }
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
        free(capture->events[i].response_id);
        free(capture->events[i].served_model);
        free(capture->events[i].route);
    }
    memset(capture, 0, sizeof(*capture));
}

/* NOLINTBEGIN(bugprone-macro-parentheses): the arguments name declared variables */
#define EVENTS_FIXTURE(capture, parser)                                                            \
    struct capture_state capture = {0};                                                            \
    struct chat_events parser;                                                                     \
    chat_events_init(&parser, capture_event, &capture)

#define EVENTS_FIXTURE_FREE(capture, parser)                                                       \
    do {                                                                                           \
        chat_events_free(&parser);                                                                 \
        reset_capture(&capture);                                                                   \
    } while (0)
/* NOLINTEND(bugprone-macro-parentheses) */

static void feed_content(struct chat_events *parser, const char *text)
{
    char data[512];
    snprintf(data, sizeof(data), "{\"choices\":[{\"delta\":{\"content\":\"%s\"}}]}", text);
    chat_events_feed(parser, data);
}

static void feed_finish(struct chat_events *parser, const char *reason)
{
    char data[512];
    snprintf(data, sizeof(data), "{\"choices\":[{\"delta\":{},\"finish_reason\":\"%s\"}]}", reason);
    chat_events_feed(parser, data);
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
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"content\":\"\"}}]}");
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"content\":null}}]}");
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}");
    EXPECT(capture.n_events == 0);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_reasoning_delta_openrouter(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning\":\"Hmm\"}}]}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_REASONING_DELTA);
    EXPECT_STR_EQ(capture.events[0].text, "Hmm");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_reasoning_delta_llamacpp(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning_content\":\"Let\"}}]}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_REASONING_DELTA);
    EXPECT_STR_EQ(capture.events[0].text, "Let");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_reasoning_then_content(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning\":\"Think\"}}]}");
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning\":\"ing\"}}]}");
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"content\":\"Answer\"}}]}");
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
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning\":\"\"}}]}");
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning\":null}}]}");
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning_content\":\"\"}}]}");
    EXPECT(capture.n_events == 0);
    EVENTS_FIXTURE_FREE(capture, parser);
}

/* Blocks accumulate in arrival order and seal at the first seam after them, before the content
 * that ended the reasoning. */
static void test_reasoning_details_sealed_at_content(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning_details\":"
                              "[{\"type\":\"reasoning.encrypted\",\"data\":\"aa\"}]}}]}");
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning\":\"Think\","
                              "\"reasoning_details\":"
                              "[{\"type\":\"reasoning.text\",\"text\":\"Think\"}]}}]}");
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"content\":\"Answer\"}}]}");

    EXPECT(capture.n_events == 3);
    EXPECT(capture.events[0].kind == EV_REASONING_DELTA);
    EXPECT(capture.events[1].kind == EV_REASONING_ITEM);
    EXPECT_STR_EQ(capture.events[1].text, "[{\"type\":\"reasoning.encrypted\",\"data\":\"aa\"},"
                                          "{\"type\":\"reasoning.text\",\"text\":\"Think\"}]");
    EXPECT(capture.events[2].kind == EV_TEXT_DELTA);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_reasoning_details_sealed_at_finish(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning_details\":"
                              "[{\"type\":\"reasoning.encrypted\",\"data\":\"aa\"}]}}]}");
    feed_finish(&parser, "stop");
    chat_events_feed(&parser, "[DONE]");

    EXPECT(capture.n_events == 2);
    EXPECT(capture.events[0].kind == EV_REASONING_ITEM);
    EXPECT_STR_EQ(capture.events[0].text, "[{\"type\":\"reasoning.encrypted\",\"data\":\"aa\"}]");
    EXPECT(capture.events[1].kind == EV_DONE);
    EVENTS_FIXTURE_FREE(capture, parser);
}

/* A block streamed one token at a time, then closed by a signature-only fragment, replays as the
 * single signed block a non-streamed response returns. */
static void test_reasoning_text_fragments_rejoined(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning_details\":"
                              "[{\"type\":\"reasoning.text\",\"text\":\"Think\","
                              "\"format\":\"anthropic-claude-v1\",\"index\":0}]}}]}");
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning_details\":"
                              "[{\"type\":\"reasoning.text\",\"text\":\"ing\",\"index\":0}]}}]}");
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning_details\":"
                              "[{\"type\":\"reasoning.text\",\"signature\":\"sig\","
                              "\"index\":0}]}}]}");
    feed_finish(&parser, "stop");

    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_REASONING_ITEM);
    json_t *details = json_loads(capture.events[0].text, 0, NULL);
    EXPECT(json_array_size(details) == 1);
    json_t *block = json_array_get(details, 0);
    EXPECT_STR_EQ(json_string_value(json_object_get(block, "text")), "Thinking");
    EXPECT_STR_EQ(json_string_value(json_object_get(block, "signature")), "sig");
    EXPECT_STR_EQ(json_string_value(json_object_get(block, "format")), "anthropic-claude-v1");
    json_decref(details);
    EVENTS_FIXTURE_FREE(capture, parser);
}

/* Only text chunks across fragments; consecutive blocks of any other type are whole already, and
 * one of them separates the text blocks around it. */
static void test_reasoning_details_join_text_only(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning_details\":"
                              "[{\"type\":\"reasoning.text\",\"text\":\"one\"},"
                              "{\"type\":\"reasoning.encrypted\",\"data\":\"aa\"},"
                              "{\"type\":\"reasoning.encrypted\",\"data\":\"bb\"},"
                              "{\"type\":\"reasoning.text\",\"text\":\"two\"}]}}]}");
    feed_finish(&parser, "stop");

    EXPECT(capture.n_events == 1);
    json_t *details = json_loads(capture.events[0].text, 0, NULL);
    EXPECT(json_array_size(details) == 4);
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(details, 0), "text")), "one");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(details, 1), "data")), "aa");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(details, 2), "data")), "bb");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(details, 3), "text")), "two");
    json_decref(details);
    EVENTS_FIXTURE_FREE(capture, parser);
}

/* The opening fragment's signature is the block's; a later one does not overwrite it. */
static void test_reasoning_text_keeps_first_signature(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning_details\":"
                              "[{\"type\":\"reasoning.text\",\"text\":\"a\","
                              "\"signature\":\"first\"},"
                              "{\"type\":\"reasoning.text\",\"text\":\"b\","
                              "\"signature\":\"second\"}]}}]}");
    feed_finish(&parser, "stop");

    json_t *details = json_loads(capture.events[0].text, 0, NULL);
    EXPECT(json_array_size(details) == 1);
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(details, 0), "text")), "ab");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(details, 0), "signature")),
                  "first");
    json_decref(details);
    EVENTS_FIXTURE_FREE(capture, parser);
}

/* An endpoint that reports no typed blocks must not gain an empty reasoning item. */
static void test_reasoning_details_absent_or_malformed(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning_details\":[]}}]}");
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning_details\":\"nope\"}}]}");
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"reasoning_details\":[\"nope\"]}}]}");
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"content\":\"Answer\"}}]}");

    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_TEXT_DELTA);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_tool_call_lifecycle(void)
{
    EVENTS_FIXTURE(capture, parser);

    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                              "\"index\":0,\"id\":\"call_1\",\"type\":\"function\","
                              "\"function\":{\"name\":\"bash\",\"arguments\":\"\"}}]}}]}");
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                              "\"index\":0,\"function\":{\"arguments\":\"{\\\"cmd\\\":\"}}]}}]}");
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                              "\"index\":0,\"function\":{\"arguments\":\"\\\"ls\\\"}\"}}]}}]}");
    feed_finish(&parser, "tool_calls");

    chat_events_feed(&parser, "[DONE]");

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
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                              "\"index\":0,\"id\":\"c1\"}]}}]}");
    EXPECT(capture.n_events == 0);
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
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
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                              "\"index\":0,\"id\":\"c1\","
                              "\"function\":{\"arguments\":\"{\\\"cmd\\\":\"}}]}}]}");
    EXPECT(capture.n_events == 0);
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
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
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":["
                              "{\"index\":0,\"id\":\"a\",\"function\":{\"name\":\"x\"}},"
                              "{\"index\":1,\"id\":\"b\",\"function\":{\"name\":\"y\"}}"
                              "]}}]}");
    feed_finish(&parser, "tool_calls");
    chat_events_feed(&parser, "[DONE]");

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
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                              "\"index\":0,\"function\":{\"name\":\"bash\","
                              "\"arguments\":\"{}\"}}]}}]}");
    feed_finish(&parser, "tool_calls");
    chat_events_feed(&parser, "[DONE]");

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
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
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
    chat_events_feed(&parser, "[DONE]");
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
    chat_events_feed(&parser, "[DONE]");
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
    chat_events_feed(&parser, "{\"choices\":[],\"usage\":{"
                              "\"prompt_tokens\":1234,\"completion_tokens\":56}}");
    chat_events_feed(&parser, "[DONE]");
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
    chat_events_finalize(&parser);
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
    chat_events_feed(&parser, "[DONE]");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_ERROR);
    EXPECT(strstr(capture.events[0].message, "content_filter") != NULL);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_done_sentinel(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "[DONE]");
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
    chat_events_feed(&parser, "[DONE]");
    chat_events_feed(&parser, "[DONE]");
    EXPECT(capture.n_events == 1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_events_after_terminal_ignored(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "[DONE]");
    feed_content(&parser, "late");
    chat_events_feed(&parser, "{\"error\":{\"message\":\"late\"}}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_DONE);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_error_object_emits_error(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "{\"error\":{\"message\":\"Rate limit exceeded\",\"code\":429}}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_ERROR);
    EXPECT_STR_EQ(capture.events[0].message, "Rate limit exceeded");
    EXPECT(parser.terminal_emitted == 1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_unparseable_json_ignored(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "not json");
    chat_events_feed(&parser, "");
    chat_events_feed(&parser, NULL);
    EXPECT(capture.n_events == 0);
    EXPECT(parser.terminal_emitted == 0);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_missing_choices_ignored(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "{\"foo\":\"bar\"}");
    chat_events_feed(&parser, "{\"choices\":[]}");
    EXPECT(capture.n_events == 0);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_finalize_without_terminal_emits_error(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_content(&parser, "hi");
    chat_events_finalize(&parser);
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
    chat_events_feed(&parser, "[DONE]");
    chat_events_finalize(&parser);
    EXPECT(capture.n_events == 1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_finalize_after_finish_without_sentinel_emits_done(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "stop");
    chat_events_finalize(&parser);
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_DONE);
    EXPECT_STR_EQ(capture.events[0].message, "stop");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_usage_default_unknown(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "stop");
    chat_events_feed(&parser, "[DONE]");
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
    chat_events_feed(&parser, "{\"choices\":[],\"usage\":{"
                              "\"prompt_tokens\":1234,\"completion_tokens\":56,"
                              "\"prompt_tokens_details\":{\"cached_tokens\":1000}}}");
    chat_events_feed(&parser, "[DONE]");
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
    chat_events_feed(&parser, "{\"choices\":[],\"usage\":{"
                              "\"prompt_tokens\":10,\"completion_tokens\":20}}");
    chat_events_feed(&parser, "[DONE]");
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
    chat_events_feed(&parser, "{\"choices\":[],\"usage\":{"
                              "\"prompt_tokens\":10,\"completion_tokens\":20,"
                              "\"cost\":0.0123}}");
    chat_events_feed(&parser, "[DONE]");
    EXPECT(capture.events[0].kind == EV_DONE);
    EXPECT(capture.events[0].usage.cost > 0.0122 && capture.events[0].usage.cost < 0.0124);
    EVENTS_FIXTURE_FREE(capture, parser);
}

/* OpenRouter repeats id, model and the upstream endpoint on every chunk. */
static void test_response_identity_captured_from_chunks(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "{\"id\":\"gen-abc\",\"model\":\"deepseek/deepseek-v4\","
                              "\"provider\":\"Wafer\",\"choices\":[{\"delta\":"
                              "{\"content\":\"hi\"}}]}");
    feed_finish(&parser, "stop");
    chat_events_feed(&parser, "[DONE]");
    EXPECT(capture.events[1].kind == EV_DONE);
    EXPECT_STR_EQ(capture.events[1].response_id, "gen-abc");
    EXPECT_STR_EQ(capture.events[1].served_model, "deepseek/deepseek-v4");
    EXPECT_STR_EQ(capture.events[1].route, "Wafer");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_response_identity_absent_without_fields(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "stop");
    chat_events_feed(&parser, "[DONE]");
    EXPECT(capture.events[0].kind == EV_DONE);
    EXPECT(!capture.events[0].response_id);
    EXPECT(!capture.events[0].served_model);
    EXPECT(!capture.events[0].route);
    EVENTS_FIXTURE_FREE(capture, parser);
}

/* The synthesized error is the only event left to carry what the first chunk identified. */
static void test_response_identity_survives_finalize_without_terminal(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "{\"id\":\"gen-cut\",\"model\":\"openai/gpt-4o-mini\","
                              "\"provider\":\"OpenAI\",\"choices\":[{\"delta\":"
                              "{\"content\":\"par\"}}]}");
    chat_events_finalize(&parser);
    EXPECT(capture.events[1].kind == EV_ERROR);
    EXPECT_STR_EQ(capture.events[1].response_id, "gen-cut");
    EXPECT_STR_EQ(capture.events[1].served_model, "openai/gpt-4o-mini");
    EXPECT_STR_EQ(capture.events[1].route, "OpenAI");
    EVENTS_FIXTURE_FREE(capture, parser);
}

/* A truncated response still leaves a usage footer, which owes the same attribution. */
static void test_response_identity_survives_truncation_error(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "{\"id\":\"gen-xyz\",\"model\":\"openai/gpt-4o-mini\","
                              "\"provider\":\"OpenAI\",\"choices\":[{\"delta\":{},"
                              "\"finish_reason\":\"length\"}]}");
    chat_events_feed(&parser, "[DONE]");
    EXPECT(capture.events[0].kind == EV_ERROR);
    EXPECT_STR_EQ(capture.events[0].response_id, "gen-xyz");
    EXPECT_STR_EQ(capture.events[0].route, "OpenAI");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_usage_cache_write_captured(void)
{
    EVENTS_FIXTURE(capture, parser);
    feed_finish(&parser, "stop");
    chat_events_feed(&parser, "{\"choices\":[],\"usage\":{"
                              "\"prompt_tokens\":2810,\"completion_tokens\":4,"
                              "\"cost\":0.01059525,"
                              "\"prompt_tokens_details\":{\"cached_tokens\":0,"
                              "\"cache_write_tokens\":2807}}}");
    chat_events_feed(&parser, "[DONE]");
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
    chat_events_feed(&parser, "{\"choices\":[],\"usage\":{"
                              "\"prompt_tokens\":2816,\"completion_tokens\":4,"
                              "\"prompt_tokens_details\":{\"cache_write_tokens\":2813}}}");
    chat_events_feed(&parser, "[DONE]");
    EXPECT(capture.events[0].usage.cache_write_tokens == 2813);
    EXPECT(capture.events[0].usage.cache_write_1h_tokens == 2813);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_usage_cache_write_1h_needs_a_write(void)
{
    EVENTS_FIXTURE(capture, parser);
    parser.cache_write_1h = 1;
    feed_finish(&parser, "stop");
    chat_events_feed(&parser, "{\"choices\":[],\"usage\":{"
                              "\"prompt_tokens\":2810,\"completion_tokens\":4,"
                              "\"prompt_tokens_details\":{\"cached_tokens\":2807}}}");
    chat_events_feed(&parser, "[DONE]");
    EXPECT(capture.events[0].usage.cached_tokens == 2807);
    EXPECT(capture.events[0].usage.cache_write_tokens == -1);
    EXPECT(capture.events[0].usage.cache_write_1h_tokens == -1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_progress_ignored_when_flag_off(void)
{
    EVENTS_FIXTURE(capture, parser);
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":{}}],"
                              "\"prompt_progress\":{\"total\":100,\"cache\":0,"
                              "\"processed\":50,\"time_ms\":42}}");
    EXPECT(capture.n_events == 0);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_progress_emitted_when_flag_on(void)
{
    EVENTS_FIXTURE(capture, parser);
    parser.emit_progress = 1;
    chat_events_feed(&parser, "{\"choices\":[{\"delta\":"
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
    chat_events_feed(&parser, "{\"choices\":[],"
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
    test_reasoning_details_sealed_at_content();
    test_reasoning_details_sealed_at_finish();
    test_reasoning_text_fragments_rejoined();
    test_reasoning_details_join_text_only();
    test_reasoning_text_keeps_first_signature();
    test_reasoning_details_absent_or_malformed();
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
    test_response_identity_captured_from_chunks();
    test_response_identity_absent_without_fields();
    test_response_identity_survives_finalize_without_terminal();
    test_response_identity_survives_truncation_error();
    test_usage_cache_write_captured();
    test_usage_cache_write_1h_attributed_from_request();
    test_usage_cache_write_1h_needs_a_write();
    test_progress_ignored_when_flag_off();
    test_progress_emitted_when_flag_on();
    test_progress_missing_fields_default_zero();
    T_REPORT();
}
