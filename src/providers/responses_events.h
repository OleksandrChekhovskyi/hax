/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_RESPONSES_EVENTS_H
#define HAX_PROVIDERS_RESPONSES_EVENTS_H

#include <stddef.h>

#include "provider.h"

struct responses_tool_call;

/* Stateful translation from OpenAI Responses SSE payloads to provider events. The parser maps
 * output-item IDs to tool-call IDs and emits at most one terminal event. */
struct responses_events {
    stream_cb callback;
    void *callback_user;
    struct responses_tool_call *tool_calls;
    size_t tool_call_count;
    size_t tool_call_capacity;
    int terminal_emitted;
};

void responses_events_init(struct responses_events *events, stream_cb callback,
                           void *callback_user);
void responses_events_free(struct responses_events *events);

/* Parse one SSE data payload. Invalid and unrecognized payloads are ignored. */
void responses_events_feed(struct responses_events *events, const char *data);

/* Emit EV_ERROR if the transport closes before a terminal response event. */
void responses_events_finalize(struct responses_events *events);

#endif /* HAX_PROVIDERS_RESPONSES_EVENTS_H */
