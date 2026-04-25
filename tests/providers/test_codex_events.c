/* SPDX-License-Identifier: MIT */
#include "providers/codex_events.h"

#include <stdlib.h>
#include <string.h>

#include "harness.h"

#define MAX_EVENTS 16

struct captured_ev {
    enum stream_event_kind kind;
    char *text;
    char *id;
    char *name;
    char *args_delta;
    char *message;
    int http_status;
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
        c->text = strdup(ev->u.reasoning_item.json);
        break;
    case EV_DONE:
        c->message = strdup(ev->u.done.stop_reason ? ev->u.done.stop_reason : "");
        break;
    case EV_ERROR:
        c->message = strdup(ev->u.error.message ? ev->u.error.message : "");
        c->http_status = ev->u.error.http_status;
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
        free(s->events[i].message);
    }
    memset(s, 0, sizeof(*s));
}

/* ---------- helpers ---------- */

#define WITH_STATE(cap, st)                                                                        \
    struct cap_state cap = {0};                                                                    \
    struct codex_events st;                                                                        \
    codex_events_init(&st, cap_cb, &cap)

#define TEARDOWN(cap, st)                                                                          \
    do {                                                                                           \
        codex_events_free(&st);                                                                    \
        cap_reset(&cap);                                                                           \
    } while (0)

/* ---------- tests ---------- */

static void test_text_delta(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.output_text.delta\",\"delta\":\"Hello\"}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_TEXT_DELTA);
    EXPECT_STR_EQ(cap.events[0].text, "Hello");
    TEARDOWN(cap, st);
}

static void test_tool_call_lifecycle(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.output_item.added\",\"item\":"
                           "{\"type\":\"function_call\",\"id\":\"i1\",\"call_id\":\"c1\","
                           "\"name\":\"bash\"}}");
    codex_events_feed(&st, "{\"type\":\"response.function_call_arguments.delta\","
                           "\"item_id\":\"i1\",\"delta\":\"chunk1\"}");
    codex_events_feed(&st, "{\"type\":\"response.function_call_arguments.delta\","
                           "\"item_id\":\"i1\",\"delta\":\"chunk2\"}");
    codex_events_feed(&st, "{\"type\":\"response.output_item.done\",\"item\":"
                           "{\"type\":\"function_call\",\"id\":\"i1\"}}");
    EXPECT(cap.n == 4);
    EXPECT(cap.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(cap.events[0].id, "c1");
    EXPECT_STR_EQ(cap.events[0].name, "bash");
    EXPECT(cap.events[1].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(cap.events[1].id, "c1");
    EXPECT_STR_EQ(cap.events[1].args_delta, "chunk1");
    EXPECT(cap.events[2].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(cap.events[2].args_delta, "chunk2");
    EXPECT(cap.events[3].kind == EV_TOOL_CALL_END);
    EXPECT_STR_EQ(cap.events[3].id, "c1");
    TEARDOWN(cap, st);
}

static void test_args_delta_for_unknown_item_dropped(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.function_call_arguments.delta\","
                           "\"item_id\":\"nope\",\"delta\":\"x\"}");
    EXPECT(cap.n == 0);
    TEARDOWN(cap, st);
}

static void test_completed_emits_done(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.completed\"}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_DONE);
    EXPECT(st.terminated == 1);
    TEARDOWN(cap, st);
}

static void test_done_sentinel(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "[DONE]");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_DONE);
    EXPECT(st.terminated == 1);
    TEARDOWN(cap, st);
}

static void test_incomplete_with_reason(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.incomplete\",\"response\":"
                           "{\"incomplete_details\":{\"reason\":\"max_output_tokens\"}}}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_ERROR);
    EXPECT(strstr(cap.events[0].message, "max_output_tokens") != NULL);
    EXPECT(st.terminated == 1);
    TEARDOWN(cap, st);
}

static void test_incomplete_without_reason(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.incomplete\"}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_ERROR);
    EXPECT(strstr(cap.events[0].message, "unknown") != NULL);
    TEARDOWN(cap, st);
}

static void test_failed_with_message(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.failed\",\"response\":"
                           "{\"error\":{\"message\":\"Bad Request\"}}}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_ERROR);
    EXPECT_STR_EQ(cap.events[0].message, "Bad Request");
    EXPECT(st.terminated == 1);
    TEARDOWN(cap, st);
}

static void test_failed_no_message_fallback(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.failed\"}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_ERROR);
    EXPECT_STR_EQ(cap.events[0].message, "response.failed");
    TEARDOWN(cap, st);
}

/* ---------- terminal event gating ---------- */

static void test_double_completion_gated(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.completed\"}");
    codex_events_feed(&st, "{\"type\":\"response.completed\"}");
    EXPECT(cap.n == 1);
    TEARDOWN(cap, st);
}

static void test_completion_then_failure_gated(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.completed\"}");
    codex_events_feed(&st, "{\"type\":\"response.failed\"}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_DONE);
    TEARDOWN(cap, st);
}

static void test_failure_then_completion_gated(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.failed\"}");
    codex_events_feed(&st, "{\"type\":\"response.completed\"}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_ERROR);
    TEARDOWN(cap, st);
}

/* ---------- malformed input ---------- */

static void test_unparseable_json_ignored(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "not json");
    codex_events_feed(&st, "");
    codex_events_feed(&st, NULL);
    EXPECT(cap.n == 0);
    EXPECT(st.terminated == 0);
    TEARDOWN(cap, st);
}

static void test_missing_type_ignored(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"foo\":\"bar\"}");
    EXPECT(cap.n == 0);
    TEARDOWN(cap, st);
}

static void test_output_item_added_non_function_call_ignored(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.output_item.added\",\"item\":"
                           "{\"type\":\"message\",\"id\":\"m1\"}}");
    EXPECT(cap.n == 0);
    TEARDOWN(cap, st);
}

static void test_tool_call_missing_required_fields_dropped(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.output_item.added\",\"item\":"
                           "{\"type\":\"function_call\",\"id\":\"i1\"}}");
    EXPECT(cap.n == 0);
    TEARDOWN(cap, st);
}

/* ---------- reasoning round-trip ---------- */

static void test_reasoning_item_emitted(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.output_item.done\",\"item\":"
                           "{\"type\":\"reasoning\",\"id\":\"rs_1\","
                           "\"summary\":[],\"encrypted_content\":\"abc==\"}}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_REASONING_ITEM);
    EXPECT(strstr(cap.events[0].text, "\"type\":\"reasoning\"") != NULL);
    EXPECT(strstr(cap.events[0].text, "\"summary\":[]") != NULL);
    EXPECT(strstr(cap.events[0].text, "\"encrypted_content\":\"abc==\"") != NULL);
    /* id is an output-only field; matches codex-rs which skips it. */
    EXPECT(strstr(cap.events[0].text, "rs_1") == NULL);
    TEARDOWN(cap, st);
}

static void test_reasoning_item_strips_unknown_fields(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.output_item.done\",\"item\":"
                           "{\"type\":\"reasoning\",\"id\":\"rs_1\","
                           "\"status\":\"completed\",\"content\":[],"
                           "\"summary\":[],\"encrypted_content\":\"abc==\","
                           "\"future_field\":\"xyz\"}}");
    EXPECT(cap.n == 1);
    EXPECT(strstr(cap.events[0].text, "\"status\"") == NULL);
    EXPECT(strstr(cap.events[0].text, "\"content\"") == NULL);
    EXPECT(strstr(cap.events[0].text, "\"future_field\"") == NULL);
    TEARDOWN(cap, st);
}

static void test_reasoning_item_without_encrypted_content_dropped(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.output_item.done\",\"item\":"
                           "{\"type\":\"reasoning\",\"id\":\"rs_1\",\"summary\":[]}}");
    EXPECT(cap.n == 0);
    TEARDOWN(cap, st);
}

static void test_reasoning_item_with_null_encrypted_content_dropped(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.output_item.done\",\"item\":"
                           "{\"type\":\"reasoning\",\"id\":\"rs_1\",\"summary\":[],"
                           "\"encrypted_content\":null}}");
    EXPECT(cap.n == 0);
    TEARDOWN(cap, st);
}

/* ---------- finalize ---------- */

static void test_finalize_without_terminal_emits_error(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.output_text.delta\",\"delta\":\"hi\"}");
    codex_events_finalize(&st);
    EXPECT(cap.n == 2);
    EXPECT(cap.events[0].kind == EV_TEXT_DELTA);
    EXPECT(cap.events[1].kind == EV_ERROR);
    EXPECT(strstr(cap.events[1].message, "stream ended") != NULL);
    EXPECT(st.terminated == 1);
    TEARDOWN(cap, st);
}

static void test_finalize_after_completed_no_extra_event(void)
{
    WITH_STATE(cap, st);
    codex_events_feed(&st, "{\"type\":\"response.completed\"}");
    codex_events_finalize(&st);
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_DONE);
    TEARDOWN(cap, st);
}

int main(void)
{
    test_text_delta();
    test_tool_call_lifecycle();
    test_args_delta_for_unknown_item_dropped();
    test_completed_emits_done();
    test_done_sentinel();
    test_incomplete_with_reason();
    test_incomplete_without_reason();
    test_failed_with_message();
    test_failed_no_message_fallback();
    test_double_completion_gated();
    test_completion_then_failure_gated();
    test_failure_then_completion_gated();
    test_unparseable_json_ignored();
    test_missing_type_ignored();
    test_output_item_added_non_function_call_ignored();
    test_reasoning_item_emitted();
    test_reasoning_item_strips_unknown_fields();
    test_reasoning_item_without_encrypted_content_dropped();
    test_reasoning_item_with_null_encrypted_content_dropped();
    test_tool_call_missing_required_fields_dropped();
    test_finalize_without_terminal_emits_error();
    test_finalize_after_completed_no_extra_event();
    T_REPORT();
}
