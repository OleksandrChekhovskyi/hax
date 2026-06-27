/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "providers/anthropic_events.h"

#define MAX_EVENTS 32

struct captured_ev {
    enum stream_event_kind kind;
    char *text;
    char *id;
    char *name;
    char *args_delta;
    char *json;
    char *message;
    struct stream_usage usage;
};

struct cap_state {
    struct captured_ev events[MAX_EVENTS];
    size_t n;
};

static int cap_cb(const struct stream_event *ev, void *user)
{
    struct cap_state *s = user;
    if (s->n >= MAX_EVENTS) {
        FAIL("%s", "too many events captured");
        return 0;
    }
    struct captured_ev *c = &s->events[s->n++];
    memset(c, 0, sizeof(*c));
    c->kind = ev->kind;
    switch (ev->kind) {
    case EV_TEXT_DELTA:
        c->text = strdup(ev->u.text_delta.text);
        break;
    case EV_TOOL_CALL_START:
        c->id = strdup(ev->u.tool_call_start.id);
        c->name = strdup(ev->u.tool_call_start.name);
        break;
    case EV_TOOL_CALL_DELTA:
        c->id = strdup(ev->u.tool_call_delta.id);
        c->args_delta = strdup(ev->u.tool_call_delta.args_delta);
        break;
    case EV_TOOL_CALL_END:
        c->id = strdup(ev->u.tool_call_end.id);
        break;
    case EV_REASONING_ITEM:
        c->json = strdup(ev->u.reasoning_item.json);
        break;
    case EV_REASONING_DELTA:
        c->text = strdup(ev->u.reasoning_delta.text ? ev->u.reasoning_delta.text : "");
        break;
    case EV_RETRY:
    case EV_PROGRESS:
        break;
    case EV_DONE:
        c->message = strdup(ev->u.done.stop_reason ? ev->u.done.stop_reason : "");
        c->usage = ev->u.done.usage;
        break;
    case EV_ERROR:
        c->message = strdup(ev->u.error.message ? ev->u.error.message : "");
        break;
    }
    return 0;
}

static void cap_reset(struct cap_state *s)
{
    for (size_t i = 0; i < s->n; i++) {
        free(s->events[i].text);
        free(s->events[i].id);
        free(s->events[i].name);
        free(s->events[i].args_delta);
        free(s->events[i].json);
        free(s->events[i].message);
    }
    memset(s, 0, sizeof(*s));
}

#define WITH_STATE(cap, st)                                                                        \
    struct cap_state cap = {0};                                                                    \
    struct anthropic_events st;                                                                    \
    anthropic_events_init(&st, cap_cb, &cap)

#define TEARDOWN(cap, st)                                                                          \
    do {                                                                                           \
        anthropic_events_free(&st);                                                                \
        cap_reset(&cap);                                                                           \
    } while (0)

#define FEED(st, json) anthropic_events_feed(&(st), NULL, (json))

/* Return the index of the first captured event of `kind`, or -1. */
static int find_kind(struct cap_state *s, enum stream_event_kind kind)
{
    for (size_t i = 0; i < s->n; i++)
        if (s->events[i].kind == kind)
            return (int)i;
    return -1;
}

/* ---------- text ---------- */

static void test_text_delta(void)
{
    WITH_STATE(cap, st);
    FEED(st, "{\"type\":\"content_block_start\",\"index\":0,"
             "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    FEED(st, "{\"type\":\"content_block_delta\",\"index\":0,"
             "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_TEXT_DELTA);
    EXPECT_STR_EQ(cap.events[0].text, "Hello");
    TEARDOWN(cap, st);
}

/* ---------- thinking round-trip ---------- */

static void test_thinking_block_assembles_reasoning_item(void)
{
    WITH_STATE(cap, st);
    /* Block start fires a state-only reasoning delta (empty text) to wake the
     * spinner even before any thinking text streams. */
    FEED(st, "{\"type\":\"content_block_start\",\"index\":0,"
             "\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_REASONING_DELTA);
    EXPECT_STR_EQ(cap.events[0].text, "");

    FEED(st, "{\"type\":\"content_block_delta\",\"index\":0,"
             "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"Let me \"}}");
    FEED(st, "{\"type\":\"content_block_delta\",\"index\":0,"
             "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"think.\"}}");
    FEED(st, "{\"type\":\"content_block_delta\",\"index\":0,"
             "\"delta\":{\"type\":\"signature_delta\",\"signature\":\"SIG123\"}}");
    FEED(st, "{\"type\":\"content_block_stop\",\"index\":0}");

    /* deltas + the reasoning item */
    int idx = find_kind(&cap, EV_REASONING_ITEM);
    EXPECT(idx >= 0);
    json_t *obj = json_loads(cap.events[idx].json, 0, NULL);
    EXPECT(obj != NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(obj, "type")), "thinking");
    EXPECT_STR_EQ(json_string_value(json_object_get(obj, "thinking")), "Let me think.");
    EXPECT_STR_EQ(json_string_value(json_object_get(obj, "signature")), "SIG123");
    json_decref(obj);
    TEARDOWN(cap, st);
}

static void test_thinking_omitted_empty_text_still_round_trips_signature(void)
{
    /* display:"omitted" — no thinking_delta text, only a signature. The block
     * must still round-trip (the signature is what the model needs back). */
    WITH_STATE(cap, st);
    FEED(st, "{\"type\":\"content_block_start\",\"index\":0,"
             "\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}");
    FEED(st, "{\"type\":\"content_block_delta\",\"index\":0,"
             "\"delta\":{\"type\":\"signature_delta\",\"signature\":\"OPAQUE\"}}");
    FEED(st, "{\"type\":\"content_block_stop\",\"index\":0}");
    int idx = find_kind(&cap, EV_REASONING_ITEM);
    EXPECT(idx >= 0);
    json_t *obj = json_loads(cap.events[idx].json, 0, NULL);
    EXPECT(obj != NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(obj, "thinking")), "");
    EXPECT_STR_EQ(json_string_value(json_object_get(obj, "signature")), "OPAQUE");
    json_decref(obj);
    TEARDOWN(cap, st);
}

static void test_redacted_thinking_round_trips_data(void)
{
    WITH_STATE(cap, st);
    FEED(st, "{\"type\":\"content_block_start\",\"index\":0,"
             "\"content_block\":{\"type\":\"redacted_thinking\",\"data\":\"ENCRYPTED\"}}");
    FEED(st, "{\"type\":\"content_block_stop\",\"index\":0}");
    int idx = find_kind(&cap, EV_REASONING_ITEM);
    EXPECT(idx >= 0);
    json_t *obj = json_loads(cap.events[idx].json, 0, NULL);
    EXPECT(obj != NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(obj, "type")), "redacted_thinking");
    EXPECT_STR_EQ(json_string_value(json_object_get(obj, "data")), "ENCRYPTED");
    json_decref(obj);
    TEARDOWN(cap, st);
}

/* ---------- tool use ---------- */

static void test_tool_use_lifecycle(void)
{
    WITH_STATE(cap, st);
    FEED(st, "{\"type\":\"content_block_start\",\"index\":0,"
             "\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_1\","
             "\"name\":\"bash\",\"input\":{}}}");
    FEED(st, "{\"type\":\"content_block_delta\",\"index\":0,"
             "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"cmd\\\":\"}}");
    FEED(st, "{\"type\":\"content_block_delta\",\"index\":0,"
             "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"ls\\\"}\"}}");
    FEED(st, "{\"type\":\"content_block_stop\",\"index\":0}");

    EXPECT(cap.n == 4);
    EXPECT(cap.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(cap.events[0].id, "toolu_1");
    EXPECT_STR_EQ(cap.events[0].name, "bash");
    EXPECT(cap.events[1].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(cap.events[1].args_delta, "{\"cmd\":");
    EXPECT(cap.events[2].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(cap.events[2].args_delta, "\"ls\"}");
    EXPECT(cap.events[3].kind == EV_TOOL_CALL_END);
    EXPECT_STR_EQ(cap.events[3].id, "toolu_1");
    TEARDOWN(cap, st);
}

/* ---------- usage + termination ---------- */

static void test_usage_and_done(void)
{
    WITH_STATE(cap, st);
    FEED(st, "{\"type\":\"message_start\",\"message\":{\"usage\":{"
             "\"input_tokens\":100,\"cache_read_input_tokens\":40,"
             "\"cache_creation_input_tokens\":10,\"output_tokens\":0}}}");
    FEED(st, "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
             "\"usage\":{\"output_tokens\":25}}");
    FEED(st, "{\"type\":\"message_stop\"}");

    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_DONE);
    EXPECT_STR_EQ(cap.events[0].message, "end_turn");
    /* total input = 100 + 40 + 10 */
    EXPECT(cap.events[0].usage.input_tokens == 150);
    EXPECT(cap.events[0].usage.output_tokens == 25);
    EXPECT(cap.events[0].usage.cached_tokens == 40);
    EXPECT(st.terminated == 1);
    TEARDOWN(cap, st);
}

static void test_stop_reason_max_tokens_is_error(void)
{
    WITH_STATE(cap, st);
    FEED(st, "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"max_tokens\"},"
             "\"usage\":{\"output_tokens\":99}}");
    FEED(st, "{\"type\":\"message_stop\"}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_ERROR);
    EXPECT(strstr(cap.events[0].message, "max_tokens") != NULL);
    TEARDOWN(cap, st);
}

static void test_stop_reason_pause_turn_is_error(void)
{
    /* pause_turn means the turn is incomplete and would need continuation —
     * it must not be mistaken for a finished answer. */
    WITH_STATE(cap, st);
    FEED(st, "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"pause_turn\"},"
             "\"usage\":{\"output_tokens\":10}}");
    FEED(st, "{\"type\":\"message_stop\"}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_ERROR);
    EXPECT(strstr(cap.events[0].message, "pause_turn") != NULL);
    TEARDOWN(cap, st);
}

static void test_error_event(void)
{
    WITH_STATE(cap, st);
    FEED(st, "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
             "\"message\":\"Overloaded\"}}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_ERROR);
    EXPECT_STR_EQ(cap.events[0].message, "Overloaded");
    EXPECT(st.terminated == 1);
    TEARDOWN(cap, st);
}

static void test_ping_and_unparseable_ignored(void)
{
    WITH_STATE(cap, st);
    FEED(st, "{\"type\":\"ping\"}");
    FEED(st, "not json");
    FEED(st, "");
    anthropic_events_feed(&st, "ping", NULL);
    EXPECT(cap.n == 0);
    EXPECT(st.terminated == 0);
    TEARDOWN(cap, st);
}

static void test_finalize_without_terminal_emits_error(void)
{
    WITH_STATE(cap, st);
    FEED(st, "{\"type\":\"content_block_start\",\"index\":0,"
             "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    FEED(st, "{\"type\":\"content_block_delta\",\"index\":0,"
             "\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}");
    anthropic_events_finalize(&st);
    EXPECT(cap.events[cap.n - 1].kind == EV_ERROR);
    EXPECT(strstr(cap.events[cap.n - 1].message, "stream ended") != NULL);
    EXPECT(st.terminated == 1);
    TEARDOWN(cap, st);
}

static void test_finalize_after_done_no_extra(void)
{
    WITH_STATE(cap, st);
    FEED(st, "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}");
    FEED(st, "{\"type\":\"message_stop\"}");
    anthropic_events_finalize(&st);
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_DONE);
    TEARDOWN(cap, st);
}

int main(void)
{
    test_text_delta();
    test_thinking_block_assembles_reasoning_item();
    test_thinking_omitted_empty_text_still_round_trips_signature();
    test_redacted_thinking_round_trips_data();
    test_tool_use_lifecycle();
    test_usage_and_done();
    test_stop_reason_max_tokens_is_error();
    test_stop_reason_pause_turn_is_error();
    test_error_event();
    test_ping_and_unparseable_ignored();
    test_finalize_without_terminal_emits_error();
    test_finalize_after_done_no_extra();
    T_REPORT();
}
