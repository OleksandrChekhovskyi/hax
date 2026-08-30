/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CHAT_EVENTS_H
#define HAX_PROVIDERS_CHAT_EVENTS_H

#include <jansson.h>
#include <stddef.h>

#include "buf.h"
#include "provider.h"

/* Stateful translator from Chat Completions SSE payloads to stream events. Only the modern
 * tool_calls shape is supported; legacy function_call deltas are ignored. */
struct chat_tool_call {
    int index;
    char *id;
    char *name;
    struct buf arguments_before_start;
    int started;
    int finished;
};

struct chat_events {
    stream_cb callback;
    void *callback_user;

    struct chat_tool_call *tool_calls;
    size_t n_tool_calls;
    size_t tool_call_capacity;

    /* Typed reasoning blocks collected since the last seam, awaiting an EV_REASONING_ITEM. */
    json_t *reasoning_details;

    /* The terminal event waits for [DONE] so a trailing usage chunk can be included. */
    int finish_received;
    char *finish_reason;
    char *finish_error;
    /* finish_error holds a transient upstream failure: withheld past [DONE] so the stream reads
     * as incomplete and is retried; emitted by finalize once the attempts are spent. */
    int finish_transient;
    struct stream_usage usage;
    /* Chunk-reported identity, first non-empty value winning: a stream serves one response, and
     * OpenRouter repeats these on every chunk. */
    char *response_id;
    char *served_model;
    char *route;
    int terminal_emitted;

    const char *length_hint; /* borrowed; appended to "length" errors */
    int cache_write_1h;
};

void chat_events_init(struct chat_events *parser, stream_cb callback, void *callback_user);
void chat_events_free(struct chat_events *parser);

/* Feed one SSE data payload: a JSON chunk or "[DONE]". Payloads after a terminal event are
 * ignored. */
void chat_events_feed(struct chat_events *parser, const char *data);

/* Whether the stream reached a terminal state worth keeping. A stream cut between the finish
 * chunk and [DONE] counts; a transient upstream failure never does — the attempt reads as died
 * mid-stream so the caller retries it. */
int chat_events_complete(const struct chat_events *parser);

/* Finish a cleanly closed transport; emit an error if no terminal state was received. */
void chat_events_finalize(struct chat_events *parser);

#endif /* HAX_PROVIDERS_CHAT_EVENTS_H */
