/* SPDX-License-Identifier: MIT */
#ifndef HAX_TURN_H
#define HAX_TURN_H

#include <stddef.h>

#include "provider.h"
#include "util.h"

/*
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

/* Look up an in-flight tool call by id — useful for display code that wants
 * to inspect args before turn_on_event consumes them on EV_TOOL_CALL_END. */
struct pending_tool *turn_find_pending(struct turn *t, const char *call_id);

/* Transfer ownership of the assembled items to the caller. After this,
 * the turn's internal item vector is empty; turn_reset remains safe. */
struct item *turn_take_items(struct turn *t, size_t *out_n);

#endif /* HAX_TURN_H */
