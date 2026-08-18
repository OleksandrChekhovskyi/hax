/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "provider.h"
#include "providers/anthropic_events.h"

#define MAX_CAPTURED_EVENTS 32

struct captured_event {
    enum stream_event_kind kind;
    char *text;
    char *id;
    char *name;
    char *args_delta;
    char *json;
    char *message;
    char *response_id;
    char *served_model;
    struct stream_usage usage;
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
        captured->json = strdup(event->u.reasoning_item.json);
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
        captured->usage = event->u.error.usage ? *event->u.error.usage
                                               : (struct stream_usage){-1, -1, -1, -1, -1, -1};
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
        free(capture->events[i].json);
        free(capture->events[i].message);
        free(capture->events[i].response_id);
        free(capture->events[i].served_model);
    }
    memset(capture, 0, sizeof(*capture));
}

/* NOLINTBEGIN(bugprone-macro-parentheses): the arguments name declared variables */
#define EVENTS_FIXTURE(capture, parser)                                                            \
    struct capture_state capture = {0};                                                            \
    struct anthropic_events parser;                                                                \
    anthropic_events_init(&parser, capture_event, &capture)

#define EVENTS_FIXTURE_FREE(capture, parser)                                                       \
    do {                                                                                           \
        anthropic_events_free(&parser);                                                            \
        reset_capture(&capture);                                                                   \
    } while (0)
/* NOLINTEND(bugprone-macro-parentheses) */

#define FEED(parser, json) anthropic_events_feed(&(parser), NULL, (json))

static int find_event(const struct capture_state *capture, enum stream_event_kind kind)
{
    for (size_t i = 0; i < capture->n_events; i++) {
        if (capture->events[i].kind == kind)
            return (int)i;
    }
    return -1;
}

static void test_text_delta(void)
{
    EVENTS_FIXTURE(capture, parser);
    FEED(parser, "{\"type\":\"content_block_start\",\"index\":0,"
                 "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    FEED(parser, "{\"type\":\"content_block_delta\",\"index\":0,"
                 "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_TEXT_DELTA);
    EXPECT_STR_EQ(capture.events[0].text, "Hello");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_thinking_block_assembles_reasoning_item(void)
{
    EVENTS_FIXTURE(capture, parser);

    FEED(parser, "{\"type\":\"content_block_start\",\"index\":0,"
                 "\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_REASONING_DELTA);
    EXPECT_STR_EQ(capture.events[0].text, "");

    FEED(parser, "{\"type\":\"content_block_delta\",\"index\":0,"
                 "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"Let me \"}}");
    FEED(parser, "{\"type\":\"content_block_delta\",\"index\":0,"
                 "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"think.\"}}");
    FEED(parser, "{\"type\":\"content_block_delta\",\"index\":0,"
                 "\"delta\":{\"type\":\"signature_delta\",\"signature\":\"SIG123\"}}");
    FEED(parser, "{\"type\":\"content_block_stop\",\"index\":0}");

    int index = find_event(&capture, EV_REASONING_ITEM);
    EXPECT(index >= 0);
    json_t *object = json_loads(capture.events[index].json, 0, NULL);
    EXPECT(object != NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(object, "type")), "thinking");
    EXPECT_STR_EQ(json_string_value(json_object_get(object, "thinking")), "Let me think.");
    EXPECT_STR_EQ(json_string_value(json_object_get(object, "signature")), "SIG123");
    json_decref(object);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_thinking_omitted_empty_text_still_round_trips_signature(void)
{
    EVENTS_FIXTURE(capture, parser);
    FEED(parser, "{\"type\":\"content_block_start\",\"index\":0,"
                 "\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}");
    FEED(parser, "{\"type\":\"content_block_delta\",\"index\":0,"
                 "\"delta\":{\"type\":\"signature_delta\",\"signature\":\"OPAQUE\"}}");
    FEED(parser, "{\"type\":\"content_block_stop\",\"index\":0}");
    int index = find_event(&capture, EV_REASONING_ITEM);
    EXPECT(index >= 0);
    json_t *object = json_loads(capture.events[index].json, 0, NULL);
    EXPECT(object != NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(object, "thinking")), "");
    EXPECT_STR_EQ(json_string_value(json_object_get(object, "signature")), "OPAQUE");
    json_decref(object);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_redacted_thinking_round_trips_data(void)
{
    EVENTS_FIXTURE(capture, parser);
    FEED(parser, "{\"type\":\"content_block_start\",\"index\":0,"
                 "\"content_block\":{\"type\":\"redacted_thinking\",\"data\":\"ENCRYPTED\"}}");
    FEED(parser, "{\"type\":\"content_block_stop\",\"index\":0}");
    int index = find_event(&capture, EV_REASONING_ITEM);
    EXPECT(index >= 0);
    json_t *object = json_loads(capture.events[index].json, 0, NULL);
    EXPECT(object != NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(object, "type")), "redacted_thinking");
    EXPECT_STR_EQ(json_string_value(json_object_get(object, "data")), "ENCRYPTED");
    json_decref(object);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_tool_use_lifecycle(void)
{
    EVENTS_FIXTURE(capture, parser);
    FEED(parser, "{\"type\":\"content_block_start\",\"index\":0,"
                 "\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_1\","
                 "\"name\":\"bash\",\"input\":{}}}");
    FEED(parser, "{\"type\":\"content_block_delta\",\"index\":0,"
                 "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"cmd\\\":\"}}");
    FEED(parser, "{\"type\":\"content_block_delta\",\"index\":0,"
                 "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"ls\\\"}\"}}");
    FEED(parser, "{\"type\":\"content_block_stop\",\"index\":0}");

    EXPECT(capture.n_events == 4);
    EXPECT(capture.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(capture.events[0].id, "toolu_1");
    EXPECT_STR_EQ(capture.events[0].name, "bash");
    EXPECT(capture.events[1].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(capture.events[1].args_delta, "{\"cmd\":");
    EXPECT(capture.events[2].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(capture.events[2].args_delta, "\"ls\"}");
    EXPECT(capture.events[3].kind == EV_TOOL_CALL_END);
    EXPECT_STR_EQ(capture.events[3].id, "toolu_1");
    EVENTS_FIXTURE_FREE(capture, parser);
}

/* message_start names the dated snapshot an alias resolved to. */
static void test_response_identity_from_message_start(void)
{
    EVENTS_FIXTURE(capture, parser);
    FEED(parser, "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_017a\","
                 "\"model\":\"claude-sonnet-4-5-20250929\",\"usage\":{\"input_tokens\":10}}}");
    FEED(parser, "{\"type\":\"message_stop\"}");
    EXPECT(capture.events[0].kind == EV_DONE);
    EXPECT_STR_EQ(capture.events[0].response_id, "msg_017a");
    EXPECT_STR_EQ(capture.events[0].served_model, "claude-sonnet-4-5-20250929");
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_usage_fragments_merge_at_done(void)
{
    EVENTS_FIXTURE(capture, parser);
    FEED(parser, "{\"type\":\"message_start\",\"message\":{\"usage\":{"
                 "\"input_tokens\":100,\"cache_read_input_tokens\":40,"
                 "\"cache_creation_input_tokens\":10,\"output_tokens\":0}}}");
    FEED(parser, "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
                 "\"usage\":{\"output_tokens\":25}}");
    FEED(parser, "{\"type\":\"message_stop\"}");

    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_DONE);
    EXPECT_STR_EQ(capture.events[0].message, "end_turn");

    EXPECT(capture.events[0].usage.input_tokens == 150);
    EXPECT(capture.events[0].usage.output_tokens == 25);
    EXPECT(capture.events[0].usage.cached_tokens == 40);
    EXPECT(capture.events[0].usage.cache_write_tokens == 10);

    EXPECT(capture.events[0].usage.cache_write_1h_tokens == -1);
    EXPECT(parser.terminal_emitted == 1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_usage_cache_creation_ttl_breakdown(void)
{
    EVENTS_FIXTURE(capture, parser);
    FEED(parser, "{\"type\":\"message_start\",\"message\":{\"usage\":{"
                 "\"input_tokens\":100,\"cache_read_input_tokens\":40,"
                 "\"cache_creation_input_tokens\":10,"
                 "\"cache_creation\":{\"ephemeral_5m_input_tokens\":3,"
                 "\"ephemeral_1h_input_tokens\":7},\"output_tokens\":0}}}");
    FEED(parser, "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
                 "\"usage\":{\"output_tokens\":25}}");
    FEED(parser, "{\"type\":\"message_stop\"}");

    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_DONE);
    EXPECT(capture.events[0].usage.cache_write_tokens == 10);
    EXPECT(capture.events[0].usage.cache_write_1h_tokens == 7);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_stop_reason_max_tokens_is_error(void)
{
    EVENTS_FIXTURE(capture, parser);
    FEED(parser, "{\"type\":\"message_start\",\"message\":{\"usage\":{"
                 "\"input_tokens\":100,\"cache_read_input_tokens\":40,"
                 "\"cache_creation_input_tokens\":10,\"output_tokens\":0}}}");
    FEED(parser, "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"max_tokens\"},"
                 "\"usage\":{\"output_tokens\":99}}");
    FEED(parser, "{\"type\":\"message_stop\"}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_ERROR);
    EXPECT(strstr(capture.events[0].message, "max_tokens") != NULL);

    EXPECT(capture.events[0].usage.input_tokens == 150);
    EXPECT(capture.events[0].usage.output_tokens == 99);
    EXPECT(capture.events[0].usage.cached_tokens == 40);
    EXPECT(capture.events[0].usage.cache_write_tokens == 10);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_stop_reason_pause_turn_is_error(void)
{
    EVENTS_FIXTURE(capture, parser);
    FEED(parser, "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"pause_turn\"},"
                 "\"usage\":{\"output_tokens\":10}}");
    FEED(parser, "{\"type\":\"message_stop\"}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_ERROR);
    EXPECT(strstr(capture.events[0].message, "pause_turn") != NULL);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_error_event(void)
{
    EVENTS_FIXTURE(capture, parser);
    FEED(parser, "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
                 "\"message\":\"Overloaded\"}}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_ERROR);
    EXPECT_STR_EQ(capture.events[0].message, "Overloaded");
    EXPECT(parser.terminal_emitted == 1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_ping_and_unparseable_ignored(void)
{
    EVENTS_FIXTURE(capture, parser);
    FEED(parser, "{\"type\":\"ping\"}");
    FEED(parser, "not json");
    FEED(parser, "");
    anthropic_events_feed(&parser, "ping", NULL);
    EXPECT(capture.n_events == 0);
    EXPECT(parser.terminal_emitted == 0);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_finalize_without_terminal_emits_error(void)
{
    EVENTS_FIXTURE(capture, parser);
    FEED(parser, "{\"type\":\"content_block_start\",\"index\":0,"
                 "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    FEED(parser, "{\"type\":\"content_block_delta\",\"index\":0,"
                 "\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}");
    anthropic_events_finalize(&parser);
    EXPECT(capture.events[capture.n_events - 1].kind == EV_ERROR);
    EXPECT(strstr(capture.events[capture.n_events - 1].message, "stream ended") != NULL);
    EXPECT(parser.terminal_emitted == 1);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_finalize_after_done_no_extra(void)
{
    EVENTS_FIXTURE(capture, parser);
    FEED(parser, "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}");
    FEED(parser, "{\"type\":\"message_stop\"}");
    anthropic_events_finalize(&parser);
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_DONE);
    EVENTS_FIXTURE_FREE(capture, parser);
}

static void test_events_after_terminal_are_ignored(void)
{
    EVENTS_FIXTURE(capture, parser);
    FEED(parser, "{\"type\":\"message_stop\"}");
    FEED(parser, "{\"type\":\"content_block_delta\",\"index\":0,"
                 "\"delta\":{\"type\":\"text_delta\",\"text\":\"late\"}}");
    EXPECT(capture.n_events == 1);
    EXPECT(capture.events[0].kind == EV_DONE);
    EVENTS_FIXTURE_FREE(capture, parser);
}

int main(void)
{
    test_text_delta();
    test_thinking_block_assembles_reasoning_item();
    test_thinking_omitted_empty_text_still_round_trips_signature();
    test_redacted_thinking_round_trips_data();
    test_tool_use_lifecycle();
    test_usage_fragments_merge_at_done();
    test_usage_cache_creation_ttl_breakdown();
    test_stop_reason_max_tokens_is_error();
    test_stop_reason_pause_turn_is_error();
    test_error_event();
    test_ping_and_unparseable_ignored();
    test_finalize_without_terminal_emits_error();
    test_finalize_after_done_no_extra();
    test_events_after_terminal_are_ignored();
    test_response_identity_from_message_start();
    T_REPORT();
}
