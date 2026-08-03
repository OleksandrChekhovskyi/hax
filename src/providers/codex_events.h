/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CODEX_EVENTS_H
#define HAX_PROVIDERS_CODEX_EVENTS_H

#include <stddef.h>

#include "provider.h"

struct codex_tool_call;

/* Stateful translation from Codex Responses SSE payloads to provider events. The parser maps
 * output-item IDs to tool-call IDs and emits at most one terminal event. */
struct codex_events {
    stream_cb callback;
    void *callback_user;
    struct codex_tool_call *tool_calls;
    size_t tool_call_count;
    size_t tool_call_capacity;
    int terminal_emitted;
};

void codex_events_init(struct codex_events *events, stream_cb callback, void *callback_user);
void codex_events_free(struct codex_events *events);

/* Parse one SSE data payload. Invalid and unrecognized payloads are ignored. */
void codex_events_feed(struct codex_events *events, const char *data);

/* Emit EV_ERROR if the transport closes before a terminal response event. */
void codex_events_finalize(struct codex_events *events);

#endif /* HAX_PROVIDERS_CODEX_EVENTS_H */
