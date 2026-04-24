/* SPDX-License-Identifier: MIT */
#ifndef HAX_CODEX_EVENTS_H
#define HAX_CODEX_EVENTS_H

#include <stddef.h>

#include "provider.h"

/*
 * Parses Codex SSE event payloads and translates them into provider-agnostic
 * stream_event callbacks. Keeps per-stream state to map item_id → call_id
 * for tool-call deltas, and gates multiple terminal events (EV_DONE, EV_ERROR)
 * so a partial-then-complete race can't double-emit.
 */

struct codex_tool_track {
    char *item_id;
    char *call_id;
    char *name;
};

struct codex_events {
    stream_cb cb;
    void *user;

    struct codex_tool_track *tools;
    size_t n_tools;
    size_t cap_tools;

    int terminated; /* any terminal event (EV_DONE or EV_ERROR) emitted */
};

void codex_events_init(struct codex_events *s, stream_cb cb, void *user);
void codex_events_free(struct codex_events *s);

/* Feed one SSE `data:` payload (the JSON blob, or "[DONE]"). */
void codex_events_feed(struct codex_events *s, const char *data);

/* Emit EV_ERROR("stream ended before completion") if no terminal event was
 * produced. Call once the SSE transport closes cleanly. */
void codex_events_finalize(struct codex_events *s);

#endif /* HAX_CODEX_EVENTS_H */
