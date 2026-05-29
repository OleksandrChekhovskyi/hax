/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "turn.h"

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

static void feed_reasoning(struct turn *t, const char *text)
{
    struct stream_event ev = {.kind = EV_REASONING_DELTA, .u.reasoning_delta = {.text = text}};
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

/* ---------- reasoning capture ---------- */

static void test_reasoning_before_tool_call(void)
{
    /* Reasoning deltas precede the tool call in the stream; the captured
     * ITEM_REASONING must land before the ITEM_TOOL_CALL so build_messages
     * can attach it as reasoning_content on the same assistant message. */
    struct turn t;
    turn_init(&t);
    feed_reasoning(&t, "I should ");
    feed_reasoning(&t, "read the file.");
    EXPECT(t.n_items == 0); /* nothing flushed while reasoning streams */
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
    /* Reasoning then plain text: ITEM_REASONING precedes the assistant
     * message. */
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
    /* The leak case: a turn that emits only reasoning (no content/tool
     * calls) before DONE must still commit the reasoning to history so it
     * can be round-tripped. */
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
    /* provider.h allows state-only reasoning events (NULL or empty text)
     * that only nudge the spinner — they must not crash on append nor
     * commit an empty reasoning item. */
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
    /* Codex streams the plaintext as reasoning deltas (spinner) and then a
     * structured reasoning item (round-trip JSON). They collapse into ONE
     * item carrying both: the JSON for re-sending, the text for display. */
    struct turn t;
    turn_init(&t);
    feed_reasoning(&t, "let me think about this");
    struct stream_event item = {.kind = EV_REASONING_ITEM,
                                .u.reasoning_item = {.json = "{\"type\":\"reasoning\"}"}};
    turn_on_event(&item, &t);
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
    /* A structured reasoning item with no preceding text deltas (encrypted
     * only, no summary) carries JSON but no display text — transcript falls
     * back to the opaque tag. */
    struct turn t;
    turn_init(&t);
    struct stream_event item = {.kind = EV_REASONING_ITEM,
                                .u.reasoning_item = {.json = "{\"type\":\"reasoning\"}"}};
    turn_on_event(&item, &t);
    feed_done(&t);

    EXPECT(t.n_items == 1);
    EXPECT(t.items[0].kind == ITEM_REASONING);
    EXPECT_STR_EQ(t.items[0].reasoning_json, "{\"type\":\"reasoning\"}");
    EXPECT(t.items[0].reasoning_text == NULL);
    turn_reset(&t);
}

static void test_flush_reasoning_preserves_partial_on_abort(void)
{
    /* Abort path: reasoning streamed but the stream errored/cancelled before
     * any text/tool event or DONE. turn_flush_reasoning commits the buffered
     * CoT so it isn't lost (the agent then adds a standalone [interrupted]
     * marker since no text carried it). */
    struct turn t;
    turn_init(&t);
    feed_reasoning(&t, "partial reasoning before the drop");
    EXPECT(t.in_reasoning == 1);
    EXPECT(t.n_items == 0);

    turn_flush_reasoning(&t);
    EXPECT(t.in_reasoning == 0);
    EXPECT(t.n_items == 1);
    EXPECT(t.items[0].kind == ITEM_REASONING);
    EXPECT_STR_EQ(t.items[0].reasoning_text, "partial reasoning before the drop");
    turn_reset(&t);
}

static void test_flush_reasoning_noop_when_empty(void)
{
    struct turn t;
    turn_init(&t);
    turn_flush_reasoning(&t); /* nothing buffered */
    EXPECT(t.n_items == 0);
    turn_reset(&t);
}

/* ---------- error path ---------- */

static void test_error_preserves_text_and_sets_flag(void)
{
    struct turn t;
    turn_init(&t);
    feed_text(&t, "partial ");
    feed_text(&t, "answer");
    feed_error(&t, "oops");
    EXPECT(t.error == 1);
    /* EV_ERROR no longer flushes — the agent does it itself with an
     * [interrupted] marker so the partial response can be preserved in
     * conversation history (so the user can ask the model to "continue"
     * on the next turn). The buffered text stays in text_buf with
     * in_text=1 until the agent calls turn_flush_text. */
    EXPECT(t.n_items == 0);
    EXPECT(t.in_text == 1);
    EXPECT(t.text_buf.data != NULL);
    EXPECT_STR_EQ(t.text_buf.data, "partial answer");
    turn_reset(&t);
}

static void test_error_then_flush_with_marker(void)
{
    /* Simulate the agent's mid-stream-error handler: stream the partial
     * text, EV_ERROR fires, agent calls turn_flush_text with the
     * [interrupted] suffix, then takes the absorbed item. Verifies the
     * model sees both the partial response and the marker. */
    struct turn t;
    turn_init(&t);
    feed_text(&t, "the answer is ");
    feed_text(&t, "42 because");
    feed_error(&t, "stream dropped");
    EXPECT(t.in_text == 1);

    turn_flush_text(&t, "\n[interrupted]");
    EXPECT(t.in_text == 0);
    EXPECT(t.n_items == 1);
    EXPECT_STR_EQ(t.items[0].text, "the answer is 42 because\n[interrupted]");
    turn_reset(&t);
}

static void test_error_after_text_flushed_by_tool_call_start(void)
{
    /* The "missed marker" case from review: text streams, then a
     * tool_call starts (which flushes the text into items and clears
     * in_text), then the stream errors before EV_TOOL_CALL_END. The
     * pending tool_call is incomplete (no args finalized) so it gets
     * discarded — the agent ends up with a complete-looking assistant
     * message and no tool_call.
     *
     * If absorb_aborted_turn ran on this state, it would return 0
     * (in_text=0 → no marker added; no completed tool_calls in items
     * → no synthesized result). The agent.c EV_ERROR branch must
     * notice the 0 return and append a standalone [interrupted]
     * marker — otherwise the next user turn sees a complete-looking
     * response with no signal it was cut short. This test pins the
     * turn-state preconditions; the marker insertion lives in agent.c. */
    struct turn t;
    turn_init(&t);
    feed_text(&t, "Let me check ");
    feed_tool_start(&t, "c1", "read");
    feed_tool_delta(&t, "c1", "{\"pa");
    feed_error(&t, "stream dropped");

    /* in_text was cleared when the tool_call started, so there's no
     * partial text for the agent's flush+marker step to act on. */
    EXPECT(t.in_text == 0);
    /* The text item was flushed into items by EV_TOOL_CALL_START. */
    EXPECT(t.n_items == 1);
    EXPECT(t.items[0].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT_STR_EQ(t.items[0].text, "Let me check ");
    /* The tool_call never finished — it's in pending, not items —
     * which is why a fallback marker is needed at the agent layer. */
    EXPECT(t.n_pending == 1);
    turn_reset(&t);
}

static void test_error_with_completed_tool_call_preserved(void)
{
    /* A tool_call that finished BEFORE the mid-stream error is in the
     * items vector. The agent will absorb it and synthesize a matching
     * [interrupted] tool_result so the conversation stays well-formed. */
    struct turn t;
    turn_init(&t);
    feed_text(&t, "let me check ");
    feed_tool_start(&t, "c1", "read");
    feed_tool_delta(&t, "c1", "{\"path\":\"x.c\"}");
    feed_tool_end(&t, "c1");
    feed_error(&t, "stream dropped");

    /* Pre-flush: items vector has [assistant_msg flushed by tool_call,
     * tool_call]. The text-before-tool was flushed when the tool call
     * started (the EV_TOOL_CALL_START flush_text path), and there's no
     * text after it, so in_text is 0 and there's nothing left for the
     * agent's flush+marker to do for text. */
    EXPECT(t.in_text == 0);
    EXPECT(t.n_items == 2);
    EXPECT(t.items[0].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT(t.items[1].kind == ITEM_TOOL_CALL);
    EXPECT_STR_EQ(t.items[1].call_id, "c1");
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
    test_reasoning_before_tool_call();
    test_reasoning_before_text();
    test_reasoning_only_turn();
    test_reasoning_state_only_deltas_ignored();
    test_reasoning_item_carries_delta_text();
    test_reasoning_item_without_deltas();
    test_flush_reasoning_preserves_partial_on_abort();
    test_flush_reasoning_noop_when_empty();
    test_tool_call_end_without_start();
    test_tool_call_delta_unknown_id_ignored();
    test_turn_find_pending();
    test_error_preserves_text_and_sets_flag();
    test_error_then_flush_with_marker();
    test_error_after_text_flushed_by_tool_call_start();
    test_error_with_completed_tool_call_preserved();
    test_take_items_zeros_vector();
    test_reset_after_take_is_noop_for_items();
    test_reset_before_done_frees_partial_state();
    T_REPORT();
}
