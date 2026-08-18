/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "provider.h"
#include "providers/responses_events.h"

#define MAX_CAPTURED_EVENTS 16

struct captured_event {
    enum stream_event_kind kind;
    char *text;
    char *id;
    char *name;
    char *args_delta;
    char *message;
    char *response_id;
    char *served_model;
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
        if (event->u.done.response.id)
            captured->response_id = strdup(event->u.done.response.id);
        if (event->u.done.response.model)
            captured->served_model = strdup(event->u.done.response.model);
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
        free(capture->events[i].response_id);
        free(capture->events[i].served_model);
    }
    memset(capture, 0, sizeof(*capture));
}

struct event_fixture {
    struct event_capture capture;
    struct responses_events parser;
};

static void fixture_init(struct event_fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    responses_events_init(&fixture->parser, capture_event, &fixture->capture);
}

static void fixture_free(struct event_fixture *fixture)
{
    responses_events_free(&fixture->parser);
    event_capture_free(&fixture->capture);
}

static void test_text_delta(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.output_text.delta\",\"delta\":\"Hello\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_TEXT_DELTA);
    EXPECT_STR_EQ(fixture.capture.events[0].text, "Hello");
    fixture_free(&fixture);
}

/* A refusal reaches the user as the assistant's answer; ignoring it would complete the response
 * with no text at all. */
static void test_refusal_delta_is_text(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.refusal.delta\",\"delta\":\"I can't help\"}");
    responses_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
    EXPECT(fixture.capture.count == 2);
    EXPECT(fixture.capture.events[0].kind == EV_TEXT_DELTA);
    EXPECT_STR_EQ(fixture.capture.events[0].text, "I can't help");
    EXPECT(fixture.capture.events[1].kind == EV_DONE);
    fixture_free(&fixture);
}

static void test_tool_call_lifecycle(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.output_item.added\",\"item\":"
                          "{\"type\":\"function_call\",\"id\":\"i1\",\"call_id\":\"c1\","
                          "\"name\":\"bash\"}}");
    responses_events_feed(&fixture.parser, "{\"type\":\"response.function_call_arguments.delta\","
                                           "\"item_id\":\"i1\",\"delta\":\"chunk1\"}");
    responses_events_feed(&fixture.parser, "{\"type\":\"response.function_call_arguments.delta\","
                                           "\"item_id\":\"i1\",\"delta\":\"chunk2\"}");
    responses_events_feed(&fixture.parser, "{\"type\":\"response.output_item.done\",\"item\":"
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
    responses_events_feed(&fixture.parser, "{\"type\":\"response.function_call_arguments.delta\","
                                           "\"item_id\":\"nope\",\"delta\":\"x\"}");
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_completed_emits_done(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_DONE);
    fixture_free(&fixture);
}

/* An early event supplies the identity even when the terminal payload omits it. */
static void test_response_identity_from_earlier_event(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.created\",\"response\":{"
                          "\"id\":\"resp_123\",\"model\":\"gpt-5.1-2025-11-13\"}}");
    responses_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
    EXPECT(fixture.capture.events[0].kind == EV_DONE);
    EXPECT_STR_EQ(fixture.capture.events[0].response_id, "resp_123");
    EXPECT_STR_EQ(fixture.capture.events[0].served_model, "gpt-5.1-2025-11-13");
    fixture_free(&fixture);
}

static void test_done_sentinel(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "[DONE]");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_DONE);
    fixture_free(&fixture);
}

static void test_incomplete_with_reason(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.incomplete\",\"response\":"
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
    responses_events_feed(&fixture.parser, "{\"type\":\"response.incomplete\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_ERROR);
    EXPECT(strstr(fixture.capture.events[0].message, "unknown") != NULL);
    fixture_free(&fixture);
}

static void test_failed_with_message(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "{\"type\":\"response.failed\",\"response\":"
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
    responses_events_feed(&fixture.parser, "{\"type\":\"response.failed\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_ERROR);
    EXPECT_STR_EQ(fixture.capture.events[0].message, "response.failed");
    fixture_free(&fixture);
}

/* A bare `error` event replaces the terminal response event rather than preceding one, so
 * swallowing it would leave the stream reported only as truncated. */
static void test_stream_error_event(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"error\",\"code\":\"rate_limit_exceeded\","
                          "\"message\":\"Rate limit reached\",\"sequence_number\":3}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_ERROR);
    EXPECT_STR_EQ(fixture.capture.events[0].message, "Rate limit reached");
    responses_events_finalize(&fixture.parser);
    EXPECT(fixture.capture.count == 1);
    fixture_free(&fixture);
}

static void test_stream_error_falls_back_to_code(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "{\"type\":\"error\",\"code\":\"server_error\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT_STR_EQ(fixture.capture.events[0].message, "server_error");
    fixture_free(&fixture);

    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "{\"type\":\"error\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT_STR_EQ(fixture.capture.events[0].message, "provider error");
    fixture_free(&fixture);
}

static void test_duplicate_completion_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
    responses_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
    EXPECT(fixture.capture.count == 1);
    fixture_free(&fixture);
}

static void test_failure_after_completion_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
    responses_events_feed(&fixture.parser, "{\"type\":\"response.failed\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_DONE);
    fixture_free(&fixture);
}

static void test_completion_after_failure_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "{\"type\":\"response.failed\"}");
    responses_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_ERROR);
    fixture_free(&fixture);
}

static void test_unparseable_json_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "not json");
    responses_events_feed(&fixture.parser, "");
    responses_events_feed(&fixture.parser, NULL);
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_missing_type_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "{\"foo\":\"bar\"}");
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_non_tool_output_item_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "{\"type\":\"response.output_item.added\",\"item\":"
                                           "{\"type\":\"message\",\"id\":\"m1\"}}");
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_incomplete_tool_call_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "{\"type\":\"response.output_item.added\",\"item\":"
                                           "{\"type\":\"function_call\",\"id\":\"i1\"}}");
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_reasoning_item_emitted(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "{\"type\":\"response.output_item.done\",\"item\":"
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
    responses_events_feed(&fixture.parser, "{\"type\":\"response.output_item.done\",\"item\":"
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
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.output_item.done\",\"item\":"
                          "{\"type\":\"reasoning\",\"id\":\"rs_1\",\"summary\":[]}}");
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_reasoning_with_null_encrypted_content_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "{\"type\":\"response.output_item.done\",\"item\":"
                                           "{\"type\":\"reasoning\",\"id\":\"rs_1\",\"summary\":[],"
                                           "\"encrypted_content\":null}}");
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_reasoning_summary_delta(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "{\"type\":\"response.reasoning_summary_text.delta\","
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
    responses_events_feed(&fixture.parser, "{\"type\":\"response.reasoning_text.delta\","
                                           "\"delta\":\"thinking\"}");
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_REASONING_DELTA);
    EXPECT_STR_EQ(fixture.capture.events[0].text, "thinking");
    fixture_free(&fixture);
}

/* Summary parts stream with no separator on the wire; a hard line break between parts keeps
 * each on its own display line. */
static void test_reasoning_part_change_emits_break(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_summary_text.delta\","
                          "\"item_id\":\"rs_1\",\"summary_index\":0,\"delta\":\"**First**\"}");
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_summary_text.delta\","
                          "\"item_id\":\"rs_1\",\"summary_index\":0,\"delta\":\" phrase\"}");
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_summary_text.delta\","
                          "\"item_id\":\"rs_1\",\"summary_index\":1,\"delta\":\"**Second**\"}");
    EXPECT(fixture.capture.count == 4);
    EXPECT_STR_EQ(fixture.capture.events[0].text, "**First**");
    EXPECT_STR_EQ(fixture.capture.events[1].text, " phrase");
    EXPECT(fixture.capture.events[2].kind == EV_REASONING_DELTA);
    EXPECT_STR_EQ(fixture.capture.events[2].text, "  \n");
    EXPECT_STR_EQ(fixture.capture.events[3].text, "**Second**");
    fixture_free(&fixture);
}

static void test_reasoning_content_part_change_emits_break(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_text.delta\","
                          "\"item_id\":\"rs_1\",\"content_index\":0,\"delta\":\"part one\"}");
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_text.delta\","
                          "\"item_id\":\"rs_1\",\"content_index\":1,\"delta\":\"part two\"}");
    EXPECT(fixture.capture.count == 3);
    EXPECT_STR_EQ(fixture.capture.events[1].text, "  \n");
    EXPECT_STR_EQ(fixture.capture.events[2].text, "part two");
    fixture_free(&fixture);
}

/* Summary and content parts of one item index independently, so an equal index across the two
 * namespaces is still a part change. */
static void test_reasoning_break_between_summary_and_content(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_summary_text.delta\","
                          "\"item_id\":\"rs_1\",\"summary_index\":0,\"delta\":\"summary\"}");
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_text.delta\","
                          "\"item_id\":\"rs_1\",\"content_index\":0,\"delta\":\"raw thought\"}");
    EXPECT(fixture.capture.count == 3);
    EXPECT_STR_EQ(fixture.capture.events[0].text, "summary");
    EXPECT_STR_EQ(fixture.capture.events[1].text, "  \n");
    EXPECT_STR_EQ(fixture.capture.events[2].text, "raw thought");
    fixture_free(&fixture);
}

/* Backends that return no encrypted content complete reasoning items without an
 * EV_REASONING_ITEM seam, so an item change needs an injected boundary too. Unidentified
 * deltas cannot be attributed to a part and never inject one. */
static void test_reasoning_break_on_unsealed_item_change(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_summary_text.delta\","
                          "\"item_id\":\"rs_1\",\"summary_index\":2,\"delta\":\"tail of one\"}");
    responses_events_feed(&fixture.parser, "{\"type\":\"response.output_item.done\",\"item\":"
                                           "{\"type\":\"reasoning\",\"id\":\"rs_1\","
                                           "\"summary\":[{}]}}");
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_summary_text.delta\","
                          "\"item_id\":\"rs_2\",\"summary_index\":0,\"delta\":\"head of two\"}");
    responses_events_feed(&fixture.parser, "{\"type\":\"response.reasoning_summary_text.delta\","
                                           "\"summary_index\":5,\"delta\":\"no item id\"}");
    EXPECT(fixture.capture.count == 4);
    EXPECT_STR_EQ(fixture.capture.events[0].text, "tail of one");
    EXPECT_STR_EQ(fixture.capture.events[1].text, "  \n");
    EXPECT_STR_EQ(fixture.capture.events[2].text, "head of two");
    EXPECT_STR_EQ(fixture.capture.events[3].text, "no item id");
    fixture_free(&fixture);
}

/* Text and tool events also close the reasoning block downstream, so a reasoning item resumed
 * after one must not get a separator on top of that seam. */
static void test_reasoning_no_break_after_external_seam(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_summary_text.delta\","
                          "\"item_id\":\"rs_1\",\"summary_index\":0,\"delta\":\"one\"}");
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.output_text.delta\",\"delta\":\"answer\"}");
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_summary_text.delta\","
                          "\"item_id\":\"rs_2\",\"summary_index\":0,\"delta\":\"two\"}");
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.output_item.added\",\"item\":"
                          "{\"type\":\"function_call\",\"id\":\"i1\",\"call_id\":\"c1\","
                          "\"name\":\"bash\"}}");
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_summary_text.delta\","
                          "\"item_id\":\"rs_3\",\"summary_index\":0,\"delta\":\"three\"}");
    EXPECT(fixture.capture.count == 5);
    EXPECT_STR_EQ(fixture.capture.events[0].text, "one");
    EXPECT(fixture.capture.events[1].kind == EV_TEXT_DELTA);
    EXPECT_STR_EQ(fixture.capture.events[2].text, "two");
    EXPECT(fixture.capture.events[3].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(fixture.capture.events[4].text, "three");
    fixture_free(&fixture);
}

/* Tool argument and end events leave an open reasoning block open downstream, so part identity
 * must survive them: a part change spanning one still needs its separator. */
static void test_reasoning_break_survives_tool_delta_interleave(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.output_item.added\",\"item\":"
                          "{\"type\":\"function_call\",\"id\":\"i1\",\"call_id\":\"c1\","
                          "\"name\":\"bash\"}}");
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_summary_text.delta\","
                          "\"item_id\":\"rs_1\",\"summary_index\":0,\"delta\":\"alpha\"}");
    responses_events_feed(&fixture.parser, "{\"type\":\"response.function_call_arguments.delta\","
                                           "\"item_id\":\"i1\",\"delta\":\"{}\"}");
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_summary_text.delta\","
                          "\"item_id\":\"rs_1\",\"summary_index\":1,\"delta\":\"beta\"}");
    responses_events_feed(&fixture.parser, "{\"type\":\"response.output_item.done\",\"item\":"
                                           "{\"type\":\"function_call\",\"id\":\"i1\"}}");
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_summary_text.delta\","
                          "\"item_id\":\"rs_1\",\"summary_index\":2,\"delta\":\"gamma\"}");
    EXPECT(fixture.capture.count == 8);
    EXPECT(fixture.capture.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(fixture.capture.events[1].text, "alpha");
    EXPECT(fixture.capture.events[2].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(fixture.capture.events[3].text, "  \n");
    EXPECT_STR_EQ(fixture.capture.events[4].text, "beta");
    EXPECT(fixture.capture.events[5].kind == EV_TOOL_CALL_END);
    EXPECT_STR_EQ(fixture.capture.events[6].text, "  \n");
    EXPECT_STR_EQ(fixture.capture.events[7].text, "gamma");
    fixture_free(&fixture);
}

/* When EV_REASONING_ITEM seals the item, that event is the boundary; injecting a separator too
 * would put a stray blank first line on the next item's block. */
static void test_reasoning_no_break_after_item_event(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_summary_text.delta\","
                          "\"item_id\":\"rs_1\",\"summary_index\":0,\"delta\":\"tail of one\"}");
    responses_events_feed(&fixture.parser, "{\"type\":\"response.output_item.done\",\"item\":"
                                           "{\"type\":\"reasoning\",\"id\":\"rs_1\","
                                           "\"summary\":[{}],\"encrypted_content\":\"abc==\"}}");
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.reasoning_summary_text.delta\","
                          "\"item_id\":\"rs_2\",\"summary_index\":0,\"delta\":\"head of two\"}");
    EXPECT(fixture.capture.count == 3);
    EXPECT_STR_EQ(fixture.capture.events[0].text, "tail of one");
    EXPECT(fixture.capture.events[1].kind == EV_REASONING_ITEM);
    EXPECT(fixture.capture.events[2].kind == EV_REASONING_DELTA);
    EXPECT_STR_EQ(fixture.capture.events[2].text, "head of two");
    fixture_free(&fixture);
}

static void test_empty_reasoning_delta_ignored(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "{\"type\":\"response.reasoning_summary_text.delta\","
                                           "\"delta\":\"\"}");
    responses_events_feed(&fixture.parser, "{\"type\":\"response.reasoning_text.delta\"}");
    EXPECT(fixture.capture.count == 0);
    fixture_free(&fixture);
}

static void test_usage_default_unknown(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
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
    responses_events_feed(&fixture.parser, "{\"type\":\"response.completed\",\"response\":{"
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
    responses_events_feed(&fixture.parser, "[DONE]");
    EXPECT(fixture.capture.events[0].kind == EV_DONE);
    EXPECT(fixture.capture.events[0].usage.input_tokens == -1);
    fixture_free(&fixture);
}

static void test_finalize_without_terminal_emits_error(void)
{
    struct event_fixture fixture;
    fixture_init(&fixture);
    responses_events_feed(&fixture.parser,
                          "{\"type\":\"response.output_text.delta\",\"delta\":\"hi\"}");
    responses_events_finalize(&fixture.parser);
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
    responses_events_feed(&fixture.parser, "{\"type\":\"response.completed\"}");
    responses_events_finalize(&fixture.parser);
    EXPECT(fixture.capture.count == 1);
    EXPECT(fixture.capture.events[0].kind == EV_DONE);
    fixture_free(&fixture);
}

int main(void)
{
    test_text_delta();
    test_refusal_delta_is_text();
    test_tool_call_lifecycle();
    test_unknown_tool_call_delta_ignored();
    test_completed_emits_done();
    test_done_sentinel();
    test_incomplete_with_reason();
    test_incomplete_without_reason();
    test_failed_with_message();
    test_failed_without_message_uses_fallback();
    test_stream_error_event();
    test_stream_error_falls_back_to_code();
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
    test_reasoning_part_change_emits_break();
    test_reasoning_content_part_change_emits_break();
    test_reasoning_break_between_summary_and_content();
    test_reasoning_break_on_unsealed_item_change();
    test_reasoning_no_break_after_external_seam();
    test_reasoning_break_survives_tool_delta_interleave();
    test_reasoning_no_break_after_item_event();
    test_empty_reasoning_delta_ignored();
    test_usage_default_unknown();
    test_completed_usage();
    test_done_sentinel_usage_unknown();
    test_finalize_without_terminal_emits_error();
    test_finalize_after_completion_is_noop();
    test_response_identity_from_earlier_event();
    T_REPORT();
}
