/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "turn.h"

static void feed_text(struct turn *t, const char *text)
{
    struct stream_event ev = {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = text}};
    turn_consume(t, &ev);
}

static void feed_tool_start(struct turn *t, const char *id, const char *name)
{
    struct stream_event ev = {.kind = EV_TOOL_CALL_START,
                              .u.tool_call_start = {.id = id, .name = name}};
    turn_consume(t, &ev);
}

static void feed_tool_delta(struct turn *t, const char *id, const char *delta)
{
    struct stream_event ev = {.kind = EV_TOOL_CALL_DELTA,
                              .u.tool_call_delta = {.id = id, .args_delta = delta}};
    turn_consume(t, &ev);
}

static void feed_tool_end(struct turn *t, const char *id)
{
    struct stream_event ev = {.kind = EV_TOOL_CALL_END, .u.tool_call_end = {.id = id}};
    turn_consume(t, &ev);
}

static void feed_reasoning(struct turn *t, const char *text)
{
    struct stream_event ev = {.kind = EV_REASONING_DELTA, .u.reasoning_delta = {.text = text}};
    turn_consume(t, &ev);
}

static void feed_done(struct turn *t)
{
    struct stream_event ev = {.kind = EV_DONE, .u.done = {.stop_reason = "completed"}};
    turn_consume(t, &ev);
}

static void feed_error(struct turn *t, const char *msg)
{
    struct stream_event ev = {.kind = EV_ERROR, .u.error = {.message = msg, .http_status = 0}};
    turn_consume(t, &ev);
}

static void free_items(struct item *items, size_t n)
{
    for (size_t i = 0; i < n; i++)
        item_free(&items[i]);
    free(items);
}

static void test_empty_stream(void)
{
    struct turn t;
    turn_init(&t);
    feed_done(&t);
    EXPECT(t.n_items == 0);
    EXPECT(t.state == TURN_DONE);
    turn_reset(&t);
}

static void test_text_deltas_flushed_on_done(void)
{
    struct turn t;
    turn_init(&t);
    feed_text(&t, "Hello, ");
    feed_text(&t, "world");
    EXPECT(t.n_items == 0);
    feed_done(&t);
    EXPECT(t.n_items == 1);
    EXPECT(t.items[0].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT_STR_EQ(t.items[0].text, "Hello, world");
    turn_reset(&t);
}

static void test_tool_call_lifecycle(void)
{
    struct turn t;
    turn_init(&t);
    feed_tool_start(&t, "c1", "bash");
    feed_tool_delta(&t, "c1", "{\"cmd\":");
    feed_tool_delta(&t, "c1", "\"echo hi\"}");
    feed_tool_end(&t, "c1");
    feed_done(&t);

    EXPECT(t.n_items == 1);
    EXPECT(t.items[0].kind == ITEM_TOOL_CALL);
    EXPECT_STR_EQ(t.items[0].call_id, "c1");
    EXPECT_STR_EQ(t.items[0].tool_name, "bash");
    EXPECT_STR_EQ(t.items[0].tool_arguments_json, "{\"cmd\":\"echo hi\"}");
    turn_reset(&t);
}

static void test_text_then_tool_then_text(void)
{
    struct turn t;
    turn_init(&t);
    feed_text(&t, "Running... ");
    feed_tool_start(&t, "c1", "bash");
    EXPECT(t.n_items == 1);
    EXPECT(t.items[0].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT_STR_EQ(t.items[0].text, "Running... ");

    feed_tool_delta(&t, "c1", "{}");
    feed_tool_end(&t, "c1");
    feed_text(&t, "Done.");
    feed_done(&t);

    EXPECT(t.n_items == 3);
    EXPECT(t.items[0].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT(t.items[1].kind == ITEM_TOOL_CALL);
    EXPECT(t.items[2].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT_STR_EQ(t.items[2].text, "Done.");
    turn_reset(&t);
}

static void test_parallel_tool_calls(void)
{
    struct turn t;
    turn_init(&t);
    feed_tool_start(&t, "c1", "bash");
    feed_tool_start(&t, "c2", "read");
    feed_tool_delta(&t, "c1", "{\"cmd\":\"ls\"}");
    feed_tool_delta(&t, "c2", "{\"path\":\"x\"}");
    feed_tool_end(&t, "c1");
    feed_tool_end(&t, "c2");
    feed_done(&t);

    EXPECT(t.n_items == 2);
    EXPECT_STR_EQ(t.items[0].call_id, "c1");
    EXPECT_STR_EQ(t.items[0].tool_name, "bash");
    EXPECT_STR_EQ(t.items[0].tool_arguments_json, "{\"cmd\":\"ls\"}");
    EXPECT_STR_EQ(t.items[1].call_id, "c2");
    EXPECT_STR_EQ(t.items[1].tool_name, "read");
    EXPECT_STR_EQ(t.items[1].tool_arguments_json, "{\"path\":\"x\"}");
    turn_reset(&t);
}

static void test_duplicate_tool_call_end_ignored(void)
{
    struct turn t;
    turn_init(&t);
    feed_tool_start(&t, "c1", "bash");
    feed_tool_end(&t, "c1");
    feed_tool_end(&t, "c1");
    feed_done(&t);
    EXPECT(t.n_items == 1);
    turn_reset(&t);
}

static void test_tool_call_end_without_start(void)
{
    struct turn t;
    turn_init(&t);
    feed_tool_end(&t, "nope");
    feed_done(&t);
    EXPECT(t.n_items == 0);
    turn_reset(&t);
}

static void test_tool_call_delta_unknown_id_ignored(void)
{
    struct turn t;
    turn_init(&t);
    feed_tool_start(&t, "c1", "bash");
    feed_tool_delta(&t, "other", "{\"x\":1}");
    feed_tool_end(&t, "c1");
    feed_done(&t);
    EXPECT(t.n_items == 1);
    EXPECT_STR_EQ(t.items[0].tool_arguments_json, "");
    turn_reset(&t);
}

static void test_reasoning_before_tool_call(void)
{
    /* Adapters attach preceding reasoning to the assistant tool-call message. */
    struct turn t;
    turn_init(&t);
    feed_reasoning(&t, "I should ");
    feed_reasoning(&t, "read the file.");
    EXPECT(t.n_items == 0);
    feed_tool_start(&t, "c1", "read");
    feed_tool_delta(&t, "c1", "{\"path\":\"x\"}");
    feed_tool_end(&t, "c1");
    feed_done(&t);

    EXPECT(t.n_items == 2);
    EXPECT(t.items[0].kind == ITEM_REASONING);
    EXPECT_STR_EQ(t.items[0].reasoning_text, "I should read the file.");
    EXPECT(t.items[0].reasoning_json == NULL);
    EXPECT(t.items[1].kind == ITEM_TOOL_CALL);
    turn_reset(&t);
}

static void test_reasoning_before_text(void)
{
    struct turn t;
    turn_init(&t);
    feed_reasoning(&t, "thinking...");
    feed_text(&t, "Here is the answer.");
    feed_done(&t);

    EXPECT(t.n_items == 2);
    EXPECT(t.items[0].kind == ITEM_REASONING);
    EXPECT_STR_EQ(t.items[0].reasoning_text, "thinking...");
    EXPECT(t.items[1].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT_STR_EQ(t.items[1].text, "Here is the answer.");
    turn_reset(&t);
}

static void test_reasoning_only_turn(void)
{
    /* Reasoning-only responses must still be available for the next request. */
    struct turn t;
    turn_init(&t);
    feed_reasoning(&t, "all my thinking went here");
    feed_done(&t);

    EXPECT(t.n_items == 1);
    EXPECT(t.items[0].kind == ITEM_REASONING);
    EXPECT_STR_EQ(t.items[0].reasoning_text, "all my thinking went here");
    turn_reset(&t);
}

static void test_reasoning_state_only_deltas_ignored(void)
{
    struct turn t;
    turn_init(&t);
    feed_reasoning(&t, NULL);
    feed_reasoning(&t, "");
    feed_text(&t, "answer");
    feed_done(&t);

    EXPECT(t.n_items == 1);
    EXPECT(t.items[0].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT_STR_EQ(t.items[0].text, "answer");
    turn_reset(&t);
}

static void test_reasoning_item_carries_delta_text(void)
{
    /* Display text and opaque round-trip state belong to the same reasoning item. */
    struct turn t;
    turn_init(&t);
    feed_reasoning(&t, "let me think about this");
    struct stream_event item = {.kind = EV_REASONING_ITEM,
                                .u.reasoning_item = {.json = "{\"type\":\"reasoning\"}"}};
    turn_consume(&t, &item);
    feed_text(&t, "the answer");
    feed_done(&t);

    EXPECT(t.n_items == 2);
    EXPECT(t.items[0].kind == ITEM_REASONING);
    EXPECT_STR_EQ(t.items[0].reasoning_json, "{\"type\":\"reasoning\"}");
    EXPECT_STR_EQ(t.items[0].reasoning_text, "let me think about this");
    EXPECT(t.items[1].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT_STR_EQ(t.items[1].text, "the answer");
    turn_reset(&t);
}

static void test_reasoning_item_without_deltas(void)
{
    struct turn t;
    turn_init(&t);
    struct stream_event item = {.kind = EV_REASONING_ITEM,
                                .u.reasoning_item = {.json = "{\"type\":\"reasoning\"}"}};
    turn_consume(&t, &item);
    feed_done(&t);

    EXPECT(t.n_items == 1);
    EXPECT(t.items[0].kind == ITEM_REASONING);
    EXPECT_STR_EQ(t.items[0].reasoning_json, "{\"type\":\"reasoning\"}");
    EXPECT(t.items[0].reasoning_text == NULL);
    turn_reset(&t);
}

static void test_flush_reasoning_preserves_partial_on_abort(void)
{
    struct turn t;
    turn_init(&t);
    feed_reasoning(&t, "partial reasoning before the drop");
    EXPECT(t.has_reasoning == 1);
    EXPECT(t.n_items == 0);

    turn_flush_reasoning(&t);
    EXPECT(t.has_reasoning == 0);
    EXPECT(t.n_items == 1);
    EXPECT(t.items[0].kind == ITEM_REASONING);
    EXPECT_STR_EQ(t.items[0].reasoning_text, "partial reasoning before the drop");
    turn_reset(&t);
}

static void test_flush_reasoning_noop_when_empty(void)
{
    struct turn t;
    turn_init(&t);
    turn_flush_reasoning(&t);
    EXPECT(t.n_items == 0);
    turn_reset(&t);
}

static void test_error_is_terminal_and_preserves_text(void)
{
    struct turn t;
    turn_init(&t);
    feed_text(&t, "partial ");
    feed_text(&t, "answer");
    feed_error(&t, "oops");
    feed_done(&t);
    feed_text(&t, " ignored");
    EXPECT(t.state == TURN_FAILED);
    EXPECT(t.n_items == 0);
    EXPECT(t.has_text == 1);
    EXPECT(t.text.data != NULL);
    EXPECT_STR_EQ(t.text.data, "partial answer");
    turn_reset(&t);
}

static void test_error_then_flush_with_marker(void)
{
    struct turn t;
    turn_init(&t);
    feed_text(&t, "the answer is ");
    feed_text(&t, "42 because");
    feed_error(&t, "stream dropped");
    EXPECT(t.has_text == 1);

    turn_flush_text(&t, "\n[interrupted]");
    EXPECT(t.has_text == 0);
    EXPECT(t.n_items == 1);
    EXPECT_STR_EQ(t.items[0].text, "the answer is 42 because\n[interrupted]");
    turn_reset(&t);
}

static void test_error_after_text_flushed_by_tool_call_start(void)
{
    /* Abort repair cannot preserve an incomplete call and must add a standalone marker. */
    struct turn t;
    turn_init(&t);
    feed_text(&t, "Let me check ");
    feed_tool_start(&t, "c1", "read");
    feed_tool_delta(&t, "c1", "{\"pa");
    feed_error(&t, "stream dropped");

    EXPECT(t.has_text == 0);
    EXPECT(t.n_items == 1);
    EXPECT(t.items[0].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT_STR_EQ(t.items[0].text, "Let me check ");
    EXPECT(t.n_pending_calls == 1);
    turn_reset(&t);
}

static void test_error_with_completed_tool_call_preserved(void)
{
    /* Completed calls remain available for abort repair to pair with results. */
    struct turn t;
    turn_init(&t);
    feed_text(&t, "let me check ");
    feed_tool_start(&t, "c1", "read");
    feed_tool_delta(&t, "c1", "{\"path\":\"x.c\"}");
    feed_tool_end(&t, "c1");
    feed_error(&t, "stream dropped");

    EXPECT(t.has_text == 0);
    EXPECT(t.n_items == 2);
    EXPECT(t.items[0].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT(t.items[1].kind == ITEM_TOOL_CALL);
    EXPECT_STR_EQ(t.items[1].call_id, "c1");
    turn_reset(&t);
}

static void test_take_items_zeros_vector(void)
{
    struct turn t;
    turn_init(&t);
    feed_text(&t, "hi");
    feed_done(&t);
    size_t n = 999;
    struct item *items = turn_take_items(&t, &n);
    EXPECT(n == 1);
    EXPECT(items != NULL);
    EXPECT_STR_EQ(items[0].text, "hi");
    EXPECT(t.items == NULL);
    EXPECT(t.n_items == 0);
    EXPECT(t.cap_items == 0);
    free_items(items, n);
    turn_reset(&t);
}

static void test_reset_after_take_is_noop_for_items(void)
{
    struct turn t;
    turn_init(&t);
    feed_tool_start(&t, "c1", "bash");
    feed_tool_delta(&t, "c1", "{}");
    feed_tool_end(&t, "c1");
    feed_done(&t);

    size_t n = 0;
    struct item *items = turn_take_items(&t, &n);
    EXPECT(n == 1);
    turn_reset(&t);
    EXPECT_STR_EQ(items[0].tool_name, "bash");
    free_items(items, n);
}

static void test_reset_before_done_frees_partial_state(void)
{
    struct turn t;
    turn_init(&t);
    feed_text(&t, "about to call a tool");
    feed_tool_start(&t, "c1", "bash");
    feed_tool_delta(&t, "c1", "{\"cmd\":\"echo\"}");
    turn_reset(&t);
    EXPECT(t.items == NULL);
    EXPECT(t.n_items == 0);
    EXPECT(t.pending_calls == NULL);
    EXPECT(t.n_pending_calls == 0);
}

int main(void)
{
    test_empty_stream();
    test_text_deltas_flushed_on_done();
    test_tool_call_lifecycle();
    test_text_then_tool_then_text();
    test_parallel_tool_calls();
    test_duplicate_tool_call_end_ignored();
    test_tool_call_end_without_start();
    test_tool_call_delta_unknown_id_ignored();
    test_reasoning_before_tool_call();
    test_reasoning_before_text();
    test_reasoning_only_turn();
    test_reasoning_state_only_deltas_ignored();
    test_reasoning_item_carries_delta_text();
    test_reasoning_item_without_deltas();
    test_flush_reasoning_preserves_partial_on_abort();
    test_flush_reasoning_noop_when_empty();
    test_error_is_terminal_and_preserves_text();
    test_error_then_flush_with_marker();
    test_error_after_text_flushed_by_tool_call_start();
    test_error_with_completed_tool_call_preserved();
    test_take_items_zeros_vector();
    test_reset_after_take_is_noop_for_items();
    test_reset_before_done_frees_partial_state();
    T_REPORT();
}
