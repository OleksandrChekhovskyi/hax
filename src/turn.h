/* SPDX-License-Identifier: MIT */
#ifndef HAX_TURN_H
#define HAX_TURN_H

#include <stddef.h>

#include "buf.h"
#include "provider.h"

/* Pure state machine that assembles one provider stream into conversation items. */

enum turn_state {
    TURN_STREAMING = 0,
    TURN_DONE,
    TURN_FAILED,
};

struct pending_tool_call;

struct turn {
    struct item *items;
    size_t n_items;
    size_t cap_items;

    struct buf text;
    int has_text;

    struct buf reasoning;
    int has_reasoning;

    struct pending_tool_call *pending_calls;
    size_t n_pending_calls;
    size_t cap_pending_calls;

    enum turn_state state;
};

void turn_init(struct turn *turn);

/* Free all owned state and leave `turn` initialized and empty. */
void turn_reset(struct turn *turn);

/* Consume one borrowed event while streaming. Terminal events make later events no-ops. */
void turn_consume(struct turn *turn, const struct stream_event *event);

/* Commit buffered assistant text, appending a non-empty suffix first when provided. */
void turn_flush_text(struct turn *turn, const char *suffix);

/* Discard buffered reasoning that never sealed into an item. Assembled ITEM_REASONINGs stay. */
void turn_discard_reasoning(struct turn *turn);

/* Discard everything but assistant text: buffered and assembled reasoning, and assembled tool
 * calls. Provider-error repair keeps only what the user saw — replaying truncated reasoning or
 * calls that never ran misleads the model on the retry. */
void turn_keep_text(struct turn *turn);

/* Transfer the assembled item vector to the caller. `out_count` may be NULL. */
struct item *turn_take_items(struct turn *turn, size_t *out_count);

#endif /* HAX_TURN_H */
