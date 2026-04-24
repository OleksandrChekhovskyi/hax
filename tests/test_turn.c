/* SPDX-License-Identifier: MIT */
#include "turn.h"

#include <stdlib.h>
#include <string.h>

#include "harness.h"

/* ---------- helpers ---------- */

static void feed_text(struct turn *t, const char *text)
{
    struct stream_event ev = {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = text}};
    turn_on_event(&ev, t);
}

static void feed_tool_start(struct turn *t, const char *id, const char *name)
{
    struct stream_event ev = {.kind = EV_TOOL_CALL_START,
                              .u.tool_call_start = {.id = id, .name = name}};
    turn_on_event(&ev, t);
}

static void feed_tool_delta(struct turn *t, const char *id, const char *delta)
{
    struct stream_event ev = {.kind = EV_TOOL_CALL_DELTA,
                              .u.tool_call_delta = {.id = id, .args_delta = delta}};
    turn_on_event(&ev, t);
}

static void feed_tool_end(struct turn *t, const char *id)
{
    struct stream_event ev = {.kind = EV_TOOL_CALL_END, .u.tool_call_end = {.id = id}};
    turn_on_event(&ev, t);
}

static void feed_done(struct turn *t)
{
    struct stream_event ev = {.kind = EV_DONE, .u.done = {.stop_reason = "completed"}};
    turn_on_event(&ev, t);
}

static void feed_error(struct turn *t, const char *msg)
{
    struct stream_event ev = {.kind = EV_ERROR, .u.error = {.message = msg, .http_status = 0}};
    turn_on_event(&ev, t);
}

static void free_items(struct item *items, size_t n)
{
    for (size_t i = 0; i < n; i++)
        item_free(&items[i]);
    free(items);
}

/* ---------- text-only turns ---------- */

static void test_empty_stream(void)
{
    struct turn t;
    turn_init(&t);
    feed_done(&t);
    EXPECT(t.n_items == 0);
    EXPECT(t.error == 0);
    turn_reset(&t);
}

static void test_text_deltas_flushed_on_done(void)
{
    struct turn t;
    turn_init(&t);
    feed_text(&t, "Hello, ");
    feed_text(&t, "world");
    EXPECT(t.n_items == 0); /* nothing flushed yet */
    feed_done(&t);
    EXPECT(t.n_items == 1);
    EXPECT(t.items[0].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT_STR_EQ(t.items[0].text, "Hello, world");
    turn_reset(&t);
}

/* ---------- tool-call lifecycle ---------- */

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
    /* text should have been flushed when the tool call started */
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
    /* Order of END events determines output order */
    EXPECT_STR_EQ(t.items[0].call_id, "c1");
    EXPECT_STR_EQ(t.items[0].tool_name, "bash");
    EXPECT_STR_EQ(t.items[0].tool_arguments_json, "{\"cmd\":\"ls\"}");
    EXPECT_STR_EQ(t.items[1].call_id, "c2");
    EXPECT_STR_EQ(t.items[1].tool_name, "read");
    EXPECT_STR_EQ(t.items[1].tool_arguments_json, "{\"path\":\"x\"}");
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
    /* No args accumulated for c1 — empty string */
    EXPECT_STR_EQ(t.items[0].tool_arguments_json, "");
    turn_reset(&t);
}

static void test_turn_find_pending(void)
{
    struct turn t;
    turn_init(&t);
    feed_tool_start(&t, "c1", "bash");
    feed_tool_delta(&t, "c1", "{\"cmd\":\"foo\"}");

    struct pending_tool *p = turn_find_pending(&t, "c1");
    EXPECT(p != NULL);
    EXPECT_STR_EQ(p->name, "bash");
    EXPECT(p->args.len == strlen("{\"cmd\":\"foo\"}"));
    EXPECT(turn_find_pending(&t, "missing") == NULL);
    EXPECT(turn_find_pending(&t, NULL) == NULL);

    turn_reset(&t);
}

/* ---------- error path ---------- */

static void test_error_flushes_text_and_sets_flag(void)
{
    struct turn t;
    turn_init(&t);
    feed_text(&t, "partial ");
    feed_text(&t, "answer");
    feed_error(&t, "oops");
    EXPECT(t.error == 1);
    /* Partial text is flushed into items — but on the error path the agent
     * discards items via turn_reset, so this is only visible if we peek. */
    EXPECT(t.n_items == 1);
    EXPECT_STR_EQ(t.items[0].text, "partial answer");
    turn_reset(&t);
}

/* ---------- take_items / reset ---------- */

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
    /* After take_items the turn shouldn't double-free; a subsequent reset
     * should just clean non-item state without touching the taken vector. */
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
    /* Items still valid — we own them now. */
    EXPECT_STR_EQ(items[0].tool_name, "bash");
    free_items(items, n);
}

static void test_reset_before_done_frees_partial_state(void)
{
    /* Abandoned mid-stream: pending tool + text buf exist. Must not leak. */
    struct turn t;
    turn_init(&t);
    feed_text(&t, "about to call a tool");
    feed_tool_start(&t, "c1", "bash");
    feed_tool_delta(&t, "c1", "{\"cmd\":\"echo\"}");
    /* No END, no DONE — simulate a cut stream. */
    turn_reset(&t);
    /* No assertions — this test passes if there's no ASan/leak complaint. */
    EXPECT(t.items == NULL);
    EXPECT(t.n_items == 0);
    EXPECT(t.pending == NULL);
    EXPECT(t.n_pending == 0);
}

int main(void)
{
    test_empty_stream();
    test_text_deltas_flushed_on_done();
    test_tool_call_lifecycle();
    test_text_then_tool_then_text();
    test_parallel_tool_calls();
    test_tool_call_end_without_start();
    test_tool_call_delta_unknown_id_ignored();
    test_turn_find_pending();
    test_error_flushes_text_and_sets_flag();
    test_take_items_zeros_vector();
    test_reset_after_take_is_noop_for_items();
    test_reset_before_done_frees_partial_state();
    T_REPORT();
}
