/* SPDX-License-Identifier: MIT */
#include "providers/openai_events.h"

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

#define WITH_STATE(cap, st)                                                                        \
    struct cap_state cap = {0};                                                                    \
    struct openai_events st;                                                                       \
    openai_events_init(&st, cap_cb, &cap)

#define TEARDOWN(cap, st)                                                                          \
    do {                                                                                           \
        openai_events_free(&st);                                                                   \
        cap_reset(&cap);                                                                           \
    } while (0)

/* ---------- helpers ---------- */

/* Compact chunk builders — real chunks have id/object/created/model too, but
 * the parser ignores those. */
static void feed_content(struct openai_events *s, const char *text)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"choices\":[{\"delta\":{\"content\":\"%s\"}}]}", text);
    openai_events_feed(s, buf);
}

static void feed_finish(struct openai_events *s, const char *reason)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"choices\":[{\"delta\":{},\"finish_reason\":\"%s\"}]}", reason);
    openai_events_feed(s, buf);
}

/* ---------- text ---------- */

static void test_text_delta(void)
{
    WITH_STATE(cap, st);
    feed_content(&st, "Hello");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_TEXT_DELTA);
    EXPECT_STR_EQ(cap.events[0].text, "Hello");
    TEARDOWN(cap, st);
}

static void test_empty_content_ignored(void)
{
    WITH_STATE(cap, st);
    openai_events_feed(&st, "{\"choices\":[{\"delta\":{\"content\":\"\"}}]}");
    openai_events_feed(&st, "{\"choices\":[{\"delta\":{\"content\":null}}]}");
    openai_events_feed(&st, "{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}");
    EXPECT(cap.n == 0);
    TEARDOWN(cap, st);
}

/* ---------- tool calls ---------- */

static void test_tool_call_lifecycle(void)
{
    WITH_STATE(cap, st);
    /* First tool_call delta carries id + name. */
    openai_events_feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                            "\"index\":0,\"id\":\"call_1\",\"type\":\"function\","
                            "\"function\":{\"name\":\"bash\",\"arguments\":\"\"}}]}}]}");
    openai_events_feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                            "\"index\":0,\"function\":{\"arguments\":\"{\\\"cmd\\\":\"}}]}}]}");
    openai_events_feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                            "\"index\":0,\"function\":{\"arguments\":\"\\\"ls\\\"}\"}}]}}]}");
    feed_finish(&st, "tool_calls");

    EXPECT(cap.n == 5);
    EXPECT(cap.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(cap.events[0].id, "call_1");
    EXPECT_STR_EQ(cap.events[0].name, "bash");
    EXPECT(cap.events[1].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(cap.events[1].id, "call_1");
    EXPECT_STR_EQ(cap.events[1].args_delta, "{\"cmd\":");
    EXPECT(cap.events[2].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(cap.events[2].args_delta, "\"ls\"}");
    EXPECT(cap.events[3].kind == EV_TOOL_CALL_END);
    EXPECT_STR_EQ(cap.events[3].id, "call_1");
    EXPECT(cap.events[4].kind == EV_DONE);
    TEARDOWN(cap, st);
}

static void test_tool_call_id_and_name_across_deltas(void)
{
    /* Spec-ambiguous corner: id arrives first, name arrives on the next
     * delta. No EV_TOOL_CALL_START should fire until both are known. */
    WITH_STATE(cap, st);
    openai_events_feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                            "\"index\":0,\"id\":\"c1\"}]}}]}");
    EXPECT(cap.n == 0);
    openai_events_feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                            "\"index\":0,\"function\":{\"name\":\"bash\"}}]}}]}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(cap.events[0].id, "c1");
    EXPECT_STR_EQ(cap.events[0].name, "bash");
    TEARDOWN(cap, st);
}

static void test_tool_call_args_before_metadata_buffered(void)
{
    /* Some backends stagger metadata: args arrive before name is known.
     * Early args must be buffered and flushed as a DELTA once START fires,
     * so the downstream turn layer sees the complete JSON. */
    WITH_STATE(cap, st);
    openai_events_feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                            "\"index\":0,\"id\":\"c1\","
                            "\"function\":{\"arguments\":\"{\\\"cmd\\\":\"}}]}}]}");
    EXPECT(cap.n == 0); /* no START yet — name unknown */
    openai_events_feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                            "\"index\":0,\"function\":{\"name\":\"bash\","
                            "\"arguments\":\"\\\"ls\\\"}\"}}]}}]}");
    EXPECT(cap.n == 3);
    EXPECT(cap.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(cap.events[0].id, "c1");
    EXPECT_STR_EQ(cap.events[0].name, "bash");
    EXPECT(cap.events[1].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(cap.events[1].args_delta, "{\"cmd\":"); /* flushed buffer */
    EXPECT(cap.events[2].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(cap.events[2].args_delta, "\"ls\"}"); /* this chunk's args */
    TEARDOWN(cap, st);
}

static void test_parallel_tool_calls(void)
{
    WITH_STATE(cap, st);
    openai_events_feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":["
                            "{\"index\":0,\"id\":\"a\",\"function\":{\"name\":\"x\"}},"
                            "{\"index\":1,\"id\":\"b\",\"function\":{\"name\":\"y\"}}"
                            "]}}]}");
    feed_finish(&st, "tool_calls");

    EXPECT(cap.n == 5);
    EXPECT(cap.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(cap.events[0].id, "a");
    EXPECT(cap.events[1].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(cap.events[1].id, "b");
    EXPECT(cap.events[2].kind == EV_TOOL_CALL_END);
    EXPECT(cap.events[3].kind == EV_TOOL_CALL_END);
    /* Order of END events should match track insertion order. */
    EXPECT_STR_EQ(cap.events[2].id, "a");
    EXPECT_STR_EQ(cap.events[3].id, "b");
    EXPECT(cap.events[4].kind == EV_DONE);
    TEARDOWN(cap, st);
}

static void test_tool_call_without_id_synthesizes(void)
{
    /* Some compat backends omit `id` entirely. Synthesize `call_<index>`
     * so the call still dispatches — the id is a round-trip token the
     * server echoes back in the tool response, any unique string works. */
    WITH_STATE(cap, st);
    openai_events_feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                            "\"index\":0,\"function\":{\"name\":\"bash\","
                            "\"arguments\":\"{}\"}}]}}]}");
    feed_finish(&st, "tool_calls");

    EXPECT(cap.n == 4);
    EXPECT(cap.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(cap.events[0].id, "call_0");
    EXPECT_STR_EQ(cap.events[0].name, "bash");
    EXPECT(cap.events[1].kind == EV_TOOL_CALL_DELTA);
    EXPECT_STR_EQ(cap.events[1].id, "call_0");
    EXPECT_STR_EQ(cap.events[1].args_delta, "{}");
    EXPECT(cap.events[2].kind == EV_TOOL_CALL_END);
    EXPECT_STR_EQ(cap.events[2].id, "call_0");
    EXPECT(cap.events[3].kind == EV_DONE);
    TEARDOWN(cap, st);
}

static void test_tool_call_delta_without_index_defaults_to_zero(void)
{
    /* Some compat servers omit `index` when streaming a single tool call.
     * Default to 0 so the tool is still dispatched. */
    WITH_STATE(cap, st);
    openai_events_feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
                            "\"id\":\"x\",\"function\":{\"name\":\"y\"}}]}}]}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_TOOL_CALL_START);
    EXPECT_STR_EQ(cap.events[0].id, "x");
    EXPECT_STR_EQ(cap.events[0].name, "y");
    TEARDOWN(cap, st);
}

/* ---------- finish_reason & termination ---------- */

static void test_finish_reason_stop_emits_done(void)
{
    WITH_STATE(cap, st);
    feed_finish(&st, "stop");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_DONE);
    EXPECT_STR_EQ(cap.events[0].message, "stop");
    EXPECT(st.terminated == 1);
    TEARDOWN(cap, st);
}

static void test_finish_reason_tool_calls_emits_done(void)
{
    WITH_STATE(cap, st);
    feed_finish(&st, "tool_calls");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_DONE);
    TEARDOWN(cap, st);
}

static void test_finish_reason_length_emits_error(void)
{
    WITH_STATE(cap, st);
    feed_finish(&st, "length");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_ERROR);
    EXPECT(strstr(cap.events[0].message, "length") != NULL);
    TEARDOWN(cap, st);
}

static void test_finish_reason_content_filter_emits_error(void)
{
    WITH_STATE(cap, st);
    feed_finish(&st, "content_filter");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_ERROR);
    EXPECT(strstr(cap.events[0].message, "content_filter") != NULL);
    TEARDOWN(cap, st);
}

static void test_done_sentinel(void)
{
    WITH_STATE(cap, st);
    openai_events_feed(&st, "[DONE]");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_DONE);
    EXPECT(st.terminated == 1);
    TEARDOWN(cap, st);
}

static void test_double_termination_gated(void)
{
    WITH_STATE(cap, st);
    feed_finish(&st, "stop");
    feed_finish(&st, "stop");
    openai_events_feed(&st, "[DONE]");
    EXPECT(cap.n == 1);
    TEARDOWN(cap, st);
}

static void test_error_object_emits_error(void)
{
    WITH_STATE(cap, st);
    openai_events_feed(&st, "{\"error\":{\"message\":\"Rate limit exceeded\",\"code\":429}}");
    EXPECT(cap.n == 1);
    EXPECT(cap.events[0].kind == EV_ERROR);
    EXPECT_STR_EQ(cap.events[0].message, "Rate limit exceeded");
    EXPECT(st.terminated == 1);
    TEARDOWN(cap, st);
}

/* ---------- malformed input ---------- */

static void test_unparseable_json_ignored(void)
{
    WITH_STATE(cap, st);
    openai_events_feed(&st, "not json");
    openai_events_feed(&st, "");
    openai_events_feed(&st, NULL);
    EXPECT(cap.n == 0);
    EXPECT(st.terminated == 0);
    TEARDOWN(cap, st);
}

static void test_missing_choices_ignored(void)
{
    WITH_STATE(cap, st);
    openai_events_feed(&st, "{\"foo\":\"bar\"}");
    openai_events_feed(&st, "{\"choices\":[]}");
    EXPECT(cap.n == 0);
    TEARDOWN(cap, st);
}

/* ---------- finalize ---------- */

static void test_finalize_without_terminal_emits_error(void)
{
    WITH_STATE(cap, st);
    feed_content(&st, "hi");
    openai_events_finalize(&st);
    EXPECT(cap.n == 2);
    EXPECT(cap.events[0].kind == EV_TEXT_DELTA);
    EXPECT(cap.events[1].kind == EV_ERROR);
    EXPECT(strstr(cap.events[1].message, "stream ended") != NULL);
    EXPECT(st.terminated == 1);
    TEARDOWN(cap, st);
}

static void test_finalize_after_done_no_extra_event(void)
{
    WITH_STATE(cap, st);
    feed_finish(&st, "stop");
    openai_events_finalize(&st);
    EXPECT(cap.n == 1);
    TEARDOWN(cap, st);
}

int main(void)
{
    test_text_delta();
    test_empty_content_ignored();
    test_tool_call_lifecycle();
    test_tool_call_id_and_name_across_deltas();
    test_tool_call_args_before_metadata_buffered();
    test_parallel_tool_calls();
    test_tool_call_without_id_synthesizes();
    test_tool_call_delta_without_index_defaults_to_zero();
    test_finish_reason_stop_emits_done();
    test_finish_reason_tool_calls_emits_done();
    test_finish_reason_length_emits_error();
    test_finish_reason_content_filter_emits_error();
    test_done_sentinel();
    test_double_termination_gated();
    test_error_object_emits_error();
    test_unparseable_json_ignored();
    test_missing_choices_ignored();
    test_finalize_without_terminal_emits_error();
    test_finalize_after_done_no_extra_event();
    T_REPORT();
}
