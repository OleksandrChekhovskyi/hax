/* SPDX-License-Identifier: MIT */
#ifndef HAX_TURN_H
#define HAX_TURN_H

#include <stddef.h>

#include "provider.h"
#include "util.h"

/*
 * Terminology: a bare "turn" is ONE model round-trip — a single provider
 * stream() call producing one assistant response (text and/or a batch of tool
 * calls). A "user turn" is the larger unit: one user prompt plus every turn it
 * spawns (the model calls tools, we run them and stream again, until a turn
 * comes back with no tool calls), so one user turn contains one or more turns.
 * ITEM_TURN_BOUNDARY marks the seam between consecutive turns; the per-user-
 * turn usage summary in agent.c aggregates token counts across them. The bare
 * "turn" = round-trip sense matches the Anthropic / OpenAI agent SDKs
 * (maxTurns / max_turns). "turn" is overloaded in the wider field — alone it
 * often means a whole user→assistant exchange — so hax keeps that broader
 * sense behind the qualified "user turn" and never bare "turn".
 *
 * Assembles one streamed model response into a list of items ready to be
 * appended to conversation history. Pure state — no I/O. Agent drives this
 * alongside display logic so the two stay decoupled.
 *
 * Lifecycle:
 *   turn_init(&t)
 *   <for each stream_event> turn_on_event(&ev, &t)
 *   on success:  items = turn_take_items(&t, &n); <append to history>
 *   on any path: turn_reset(&t)  // safe even if items were taken
 */

struct pending_tool {
    char *call_id;
    char *name;
    struct buf args;
};

struct turn {
    struct item *items;
    size_t n_items;
    size_t cap_items;

    int in_text;
    struct buf text_buf;

    int in_reasoning;
    struct buf reasoning_buf;

    struct pending_tool *pending;
    size_t n_pending;
    size_t cap_pending;

    int error;
};

void turn_init(struct turn *t);

/* Frees all owned state, including any items still held (error path). */
void turn_reset(struct turn *t);

/* Update state for one event. Returns 0 (reserved for future cancellation). */
int turn_on_event(const struct stream_event *ev, struct turn *t);

/* Finalize any in-flight assistant text into an item, optionally appending
 * `suffix` first. No-op if no text was buffered. Used by the agent on
 * user-interrupt to capture the partial assistant message and tag it with
 * "[interrupted]" so the next turn carries the marker. Pending
 * (incomplete) tool calls are NOT promoted — they are discarded by
 * turn_reset, since incomplete args wouldn't form valid tool_calls. */
void turn_flush_text(struct turn *t, const char *suffix);

/* Commit any buffered reasoning as an ITEM_REASONING. No-op if none is
 * buffered. Used on the abort/cancel path to preserve a reasoning-only
 * partial stream (CoT streamed, then error/cancel before any text/tool
 * event or EV_DONE) — otherwise the buffered reasoning would be discarded
 * by turn_reset. In the normal flow reasoning is flushed automatically when
 * the first text/tool-call event arrives or on EV_DONE. */
void turn_flush_reasoning(struct turn *t);

/* Look up an in-flight tool call by id — useful for display code that wants
 * to inspect args before turn_on_event consumes them on EV_TOOL_CALL_END. */
struct pending_tool *turn_find_pending(struct turn *t, const char *call_id);

/* Transfer ownership of the assembled items to the caller. After this,
 * the turn's internal item vector is empty; turn_reset remains safe. */
struct item *turn_take_items(struct turn *t, size_t *out_n);

#endif /* HAX_TURN_H */
