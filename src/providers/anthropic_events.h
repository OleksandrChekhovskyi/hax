/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_ANTHROPIC_EVENTS_H
#define HAX_PROVIDERS_ANTHROPIC_EVENTS_H

#include <stddef.h>

#include "buf.h"
#include "provider.h"

enum anthropic_content_kind {
    ANTHROPIC_CONTENT_TEXT = 0,
    ANTHROPIC_CONTENT_THINKING,
    ANTHROPIC_CONTENT_REDACTED_THINKING,
    ANTHROPIC_CONTENT_TOOL_USE,
    ANTHROPIC_CONTENT_OTHER,
};

struct anthropic_content_block {
    int index;
    enum anthropic_content_kind kind;
    char *tool_call_id;
    char *tool_name;
    int tool_started;
    struct buf thinking;
    struct buf signature;
    char *redacted_data;
};

/* Stateful translator from Messages API SSE payloads to stream events. Content state is keyed by
 * block index because thinking signatures and tool arguments arrive in later deltas. */
struct anthropic_events {
    stream_cb callback;
    void *callback_user;

    struct anthropic_content_block *blocks;
    size_t n_blocks;
    size_t block_capacity;

    /* Input/cache usage arrives at message_start and output usage at message_delta. */
    struct stream_usage usage;
    /* Identity arrives with message_start; the model resolves an alias to a dated snapshot. */
    char *response_id;
    char *served_model;
    char *stop_reason;
    int terminal_emitted;
};

void anthropic_events_init(struct anthropic_events *parser, stream_cb callback,
                           void *callback_user);
void anthropic_events_free(struct anthropic_events *parser);

/* Feed one SSE payload. The JSON type is authoritative; `event_name` may be NULL. Payloads after a
 * terminal event are ignored. */
void anthropic_events_feed(struct anthropic_events *parser, const char *event_name,
                           const char *data);

/* Finish a cleanly closed transport; emit an error if no terminal state was received. */
void anthropic_events_finalize(struct anthropic_events *parser);

#endif /* HAX_PROVIDERS_ANTHROPIC_EVENTS_H */
