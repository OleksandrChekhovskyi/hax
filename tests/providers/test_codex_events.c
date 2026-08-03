/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "provider.h"
#include "providers/codex_events.h"

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
};

struct event_capture {
    struct captured_event events[MAX_CAPTURED_EVENTS];
    size_t count;
};

static int capture_event(const struct stream_event *event, void *callback_user)
{
    struct event_capture *capture = callback_user;
    if (capture->count >= MAX_CAPTURED_EVENTS) {
        FAIL("%s", "too many events captured");
        return 0;
    }
    struct captured_event *captured = &capture->events[capture->count++];
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
    case EV_PROGRESS:
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
            captured->usage = (struct stream_usage){
                .input_tokens = -1,
                .output_tokens = -1,
                .cached_tokens = -1,
                .cache_write_tokens = -1,
                .cache_write_1h_tokens = -1,
                .cost = -1,
            };
        break;
    }
    return 0;
}

static void event_capture_free(struct event_capture *capture)
{
    for (size_t i = 0; i < capture->count; i++) {
        free(capture->events[i].text);
        free(capture->events[i].id);
        free(capture->events[i].name);
        free(capture->events[i].args_delta);
        free(capture->events[i].message);
    }
    memset(capture, 0, sizeof(*capture));
}

struct event_fixture {
    struct event_capture capture;
    struct codex_events parser;
};

static void fixture_init(struct event_fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    codex_events_init(&fixture->parser, capture_event, &fixture->capture);
}

static void fixture_free(struct event_fixture *fixture)
{
    codex_events_free(&fixture->parser);
    event_capture_free(&fixture->capture);
}

static void test_text_delta(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser,
                      "{\"type\":\"response.output_text.delta\",\"delta\":\"Hello\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_TEXT_DELTA);
    EXPECT_STR_EQ(fixture.capture.events[0].text, "Hello");
    fixture_free(&fixture);
}

static void test_tool_call_lifecycle(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser,
                      "{\"type\":\"response.output_item.added\",\"item\":"
                      "{\"type\":\"function_call\",\"id\":\"i1\",\"call_id\":\"c1\","
                      "\"name\":\"bash\"}}");
    codex_events_feed(&fixture.parser, "{\"type\":\"response.function_call_arguments.delta\","
                                       "\"item_id\":\"i1\",\"delta\":\"chunk1\"}");
    codex_events_feed(&fixture.parser, "{\"type\":\"response.function_call_arguments.delta\","
                                       "\"item_id\":\"i1\",\"delta\":\"chunk2\"}");
    codex_events_feed(&fixture.parser, "{\"type\":\"response.output_item.done\",\"item\":"
                                       "{\"type\":\"function_call\",\"id\":\"i1\"}}");
    EXPECT(fixture.capture.count == 4);
    EXPECT(fixture.capture.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(fixture.capture.events[0].id, "c1");
    EXPECT_STR_EQ(fixture.capture.events[0].name, "bash");
    EXPECT(fixture.capture.events[1].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(fixture.capture.events[1].id, "c1");
    EXPECT_STR_EQ(fixture.capture.events[1].args_delta, "chunk1");
    EXPECT(fixture.capture.events[2].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(fixture.capture.events[2].args_delta, "chunk2");
    EXPECT(fixture.capture.events[3].kind == EV_TOOL_CALL_END);
    EXPECT_STR_EQ(fixture.capture.events[3].id, "c1");
    fixture_free(&fixture);
}

static void test_unknown_tool_call_delta_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.function_call_arguments.delta\","
                                       "\"item_id\":\"nope\",\"delta\":\"x\"}");
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_completed_emits_done(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_DONE);
    fixture_free(&fixture);
}

static void test_done_sentinel(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "[DONE]");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_DONE);
    fixture_free(&fixture);
}

static void test_incomplete_with_reason(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.incomplete\",\"response\":"
                                       "{\"incomplete_details\":{\"reason\":\"max_output_tokens\"},"
                                       "\"usage\":{\"input_tokens\":500,\"output_tokens\":80,"
                                       "\"input_tokens_details\":{\"cached_tokens\":300}}}}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_ERROR);
    EXPECT(strstr(fixture.capture.events[0].message, "max_output_tokens") != NULL);
    EXPECT(fixture.capture.events[0].usage.input_tokens == 500);
    EXPECT(fixture.capture.events[0].usage.output_tokens == 80);
    EXPECT(fixture.capture.events[0].usage.cached_tokens == 300);
    fixture_free(&fixture);
}

static void test_incomplete_without_reason(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.incomplete\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_ERROR);
    EXPECT(strstr(fixture.capture.events[0].message, "unknown") != NULL);
    fixture_free(&fixture);
}

static void test_failed_with_message(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.failed\",\"response\":"
                                       "{\"error\":{\"message\":\"Bad Request\"}}}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_ERROR);
    EXPECT_STR_EQ(fixture.capture.events[0].message, "Bad Request");
    fixture_free(&fixture);
}

static void test_failed_without_message_uses_fallback(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.failed\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_ERROR);
    EXPECT_STR_EQ(fixture.capture.events[0].message, "response.failed");
    fixture_free(&fixture);
}

static void test_duplicate_completion_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
    codex_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
    EXPECT(fixture.capture.count == 1);
    fixture_free(&fixture);
}

static void test_failure_after_completion_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
    codex_events_feed(&fixture.parser, "{\"type\":\"response.failed\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_DONE);
    fixture_free(&fixture);
}

static void test_completion_after_failure_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.failed\"}");
    codex_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_ERROR);
    fixture_free(&fixture);
}

static void test_unparseable_json_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "not json");
    codex_events_feed(&fixture.parser, "");
    codex_events_feed(&fixture.parser, NULL);
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_missing_type_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"foo\":\"bar\"}");
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_non_tool_output_item_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.output_item.added\",\"item\":"
                                       "{\"type\":\"message\",\"id\":\"m1\"}}");
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_incomplete_tool_call_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.output_item.added\",\"item\":"
                                       "{\"type\":\"function_call\",\"id\":\"i1\"}}");
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_reasoning_item_emitted(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.output_item.done\",\"item\":"
                                       "{\"type\":\"reasoning\",\"id\":\"rs_1\","
                                       "\"summary\":[],\"encrypted_content\":\"abc==\"}}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_REASONING_ITEM);
    EXPECT(strstr(fixture.capture.events[0].text, "\"type\":\"reasoning\"") != NULL);
    EXPECT(strstr(fixture.capture.events[0].text, "\"summary\":[]") != NULL);
    EXPECT(strstr(fixture.capture.events[0].text, "\"encrypted_content\":\"abc==\"") != NULL);
    fixture_free(&fixture);
}

static void test_reasoning_item_strips_unknown_fields(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.output_item.done\",\"item\":"
                                       "{\"type\":\"reasoning\",\"id\":\"rs_1\","
                                       "\"status\":\"completed\",\"content\":[],"
                                       "\"summary\":[],\"encrypted_content\":\"abc==\","
                                       "\"future_field\":\"xyz\"}}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(strstr(fixture.capture.events[0].text, "\"status\"") == NULL);
    EXPECT(strstr(fixture.capture.events[0].text, "\"content\"") == NULL);
    EXPECT(strstr(fixture.capture.events[0].text, "\"future_field\"") == NULL);
    EXPECT(strstr(fixture.capture.events[0].text, "rs_1") == NULL);
    fixture_free(&fixture);
}

static void test_reasoning_without_encrypted_content_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.output_item.done\",\"item\":"
                                       "{\"type\":\"reasoning\",\"id\":\"rs_1\",\"summary\":[]}}");
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_reasoning_with_null_encrypted_content_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.output_item.done\",\"item\":"
                                       "{\"type\":\"reasoning\",\"id\":\"rs_1\",\"summary\":[],"
                                       "\"encrypted_content\":null}}");
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_reasoning_summary_delta(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.reasoning_summary_text.delta\","
                                       "\"delta\":\"Let's see\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_REASONING_DELTA);
    EXPECT_STR_EQ(fixture.capture.events[0].text, "Let's see");
    fixture_free(&fixture);
}

static void test_reasoning_text_delta(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.reasoning_text.delta\","
                                       "\"delta\":\"thinking\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_REASONING_DELTA);
    EXPECT_STR_EQ(fixture.capture.events[0].text, "thinking");
    fixture_free(&fixture);
}

static void test_empty_reasoning_delta_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.reasoning_summary_text.delta\","
                                       "\"delta\":\"\"}");
    codex_events_feed(&fixture.parser, "{\"type\":\"response.reasoning_text.delta\"}");
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_usage_default_unknown(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
    EXPECT(fixture.capture.events[0].kind == EV_DONE);
    EXPECT(fixture.capture.events[0].usage.input_tokens == -1);
    EXPECT(fixture.capture.events[0].usage.output_tokens == -1);
    EXPECT(fixture.capture.events[0].usage.cached_tokens == -1);
    fixture_free(&fixture);
}

static void test_completed_usage(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.completed\",\"response\":{"
                                       "\"usage\":{\"input_tokens\":2048,\"output_tokens\":128,"
                                       "\"input_tokens_details\":{\"cached_tokens\":1024}}}}");
    EXPECT(fixture.capture.events[0].kind == EV_DONE);
    EXPECT(fixture.capture.events[0].usage.input_tokens == 2048);
    EXPECT(fixture.capture.events[0].usage.output_tokens == 128);
    EXPECT(fixture.capture.events[0].usage.cached_tokens == 1024);
    fixture_free(&fixture);
}

static void test_done_sentinel_usage_unknown(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "[DONE]");
    EXPECT(fixture.capture.events[0].kind == EV_DONE);
    EXPECT(fixture.capture.events[0].usage.input_tokens == -1);
    fixture_free(&fixture);
}

static void test_finalize_without_terminal_emits_error(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser,
                      "{\"type\":\"response.output_text.delta\",\"delta\":\"hi\"}");
    codex_events_finalize(&fixture.parser);
    EXPECT(fixture.capture.count == 2);
    EXPECT(fixture.capture.events[0].kind == EV_TEXT_DELTA);
    EXPECT(fixture.capture.events[1].kind == EV_ERROR);
    EXPECT(strstr(fixture.capture.events[1].message, "stream ended") != NULL);
    fixture_free(&fixture);
}

static void test_finalize_after_completion_is_noop(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    codex_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
    codex_events_finalize(&fixture.parser);
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_DONE);
    fixture_free(&fixture);
}

int main(void)
{
    test_text_delta();
    test_tool_call_lifecycle();
    test_unknown_tool_call_delta_ignored();
    test_completed_emits_done();
    test_done_sentinel();
    test_incomplete_with_reason();
    test_incomplete_without_reason();
    test_failed_with_message();
    test_failed_without_message_uses_fallback();
    test_duplicate_completion_ignored();
    test_failure_after_completion_ignored();
    test_completion_after_failure_ignored();
    test_unparseable_json_ignored();
    test_missing_type_ignored();
    test_non_tool_output_item_ignored();
    test_incomplete_tool_call_ignored();
    test_reasoning_item_emitted();
    test_reasoning_item_strips_unknown_fields();
    test_reasoning_without_encrypted_content_ignored();
    test_reasoning_with_null_encrypted_content_ignored();
    test_reasoning_summary_delta();
    test_reasoning_text_delta();
    test_empty_reasoning_delta_ignored();
    test_usage_default_unknown();
    test_completed_usage();
    test_done_sentinel_usage_unknown();
    test_finalize_without_terminal_emits_error();
    test_finalize_after_completion_is_noop();
    T_REPORT();
}
