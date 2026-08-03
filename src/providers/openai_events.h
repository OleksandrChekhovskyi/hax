/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_OPENAI_EVENTS_H
#define HAX_PROVIDERS_OPENAI_EVENTS_H

#include <stddef.h>

#include "provider.h"
#include "util.h"

/* Stateful translator from Chat Completions SSE payloads to stream events. Only the modern
 * tool_calls shape is supported; legacy function_call deltas are ignored. */
struct openai_tool_call {
    int index;
    char *id;
    char *name;
    struct buf arguments_before_start;
    int started;
    int finished;
};

struct openai_events {
    stream_cb callback;
    void *callback_user;

    struct openai_tool_call *tool_calls;
    size_t n_tool_calls;
    size_t tool_call_capacity;

    /* The terminal event waits for [DONE] so a trailing usage chunk can be included. */
    int finish_received;
    char *finish_reason;
    char *finish_error;
    struct stream_usage usage;
    int terminal_emitted;

    int emit_progress;
    const char *length_hint; /* borrowed; appended to "length" errors */
    int cache_write_1h;
};

void openai_events_init(struct openai_events *parser, stream_cb callback, void *callback_user);
void openai_events_free(struct openai_events *parser);

/* Feed one SSE data payload: a JSON chunk or "[DONE]". Payloads after a terminal event are
 * ignored. */
void openai_events_feed(struct openai_events *parser, const char *data);

/* Finish a cleanly closed transport; emit an error if no terminal state was received. */
void openai_events_finalize(struct openai_events *parser);

#endif /* HAX_PROVIDERS_OPENAI_EVENTS_H */
