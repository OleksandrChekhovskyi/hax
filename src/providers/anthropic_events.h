/* SPDX-License-Identifier: MIT */
#ifndef HAX_ANTHROPIC_EVENTS_H
#define HAX_ANTHROPIC_EVENTS_H

#include <stddef.h>

#include "provider.h"
#include "util.h"

/*
 * Parses Anthropic Messages API streaming events and translates them into
 * provider-agnostic stream_event callbacks. The Messages stream is a sequence
 * of named SSE events (message_start, content_block_start/delta/stop,
 * message_delta, message_stop, ping, error); each `data:` payload is JSON
 * carrying a "type" field that mirrors the event name — we dispatch on that.
 *
 * Content blocks arrive sequentially (start → deltas → stop) and are keyed by
 * `index`. We track per-index state so a thinking block can accumulate its
 * text and `signature` (assembled into one EV_REASONING_ITEM at stop, ready to
 * round-trip), and a tool_use block can map its index to the call id for delta
 * forwarding.
 *
 * Usage is split across the stream: input/cache counts land in message_start,
 * the final output count in message_delta. We hold both until message_stop
 * fires EV_DONE.
 */

enum anthropic_block_kind {
    ANTHROPIC_BLK_TEXT = 0,
    ANTHROPIC_BLK_THINKING,
    ANTHROPIC_BLK_REDACTED,
    ANTHROPIC_BLK_TOOL,
    ANTHROPIC_BLK_OTHER,
};

struct anthropic_block_track {
    int index;
    enum anthropic_block_kind kind;
    /* TOOL: */
    char *tool_id;
    char *tool_name;
    int start_emitted;
    /* THINKING: accumulated plaintext + encrypted signature. */
    struct buf thinking;
    struct buf signature;
    /* REDACTED: opaque payload carried on the content_block_start. */
    char *redacted_data;
};

struct anthropic_events {
    stream_cb cb;
    void *user;

    struct anthropic_block_track *blocks;
    size_t n_blocks;
    size_t cap_blocks;

    /* Usage is assembled incrementally: input_tokens + cache_* from
     * message_start, output_tokens from message_delta. -1 = unreported. */
    struct stream_usage pending_usage;
    char *stop_reason; /* from message_delta.delta.stop_reason */

    int terminated; /* any terminal event (EV_DONE or EV_ERROR) emitted */
};

void anthropic_events_init(struct anthropic_events *s, stream_cb cb, void *user);
void anthropic_events_free(struct anthropic_events *s);

/* Feed one SSE `data:` payload (the JSON event object). `event_name` is the
 * SSE `event:` line (may be NULL); it is informational — dispatch keys off the
 * payload's own "type" field, which Anthropic always includes. */
void anthropic_events_feed(struct anthropic_events *s, const char *event_name, const char *data);

/* Emit EV_ERROR("stream ended before completion") if no terminal event was
 * produced. Call once the SSE transport closes cleanly. */
void anthropic_events_finalize(struct anthropic_events *s);

#endif /* HAX_ANTHROPIC_EVENTS_H */
