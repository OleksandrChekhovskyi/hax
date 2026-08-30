/* SPDX-License-Identifier: MIT */
#ifndef HAX_TRANSPORT_SSE_H
#define HAX_TRANSPORT_SSE_H

#include <stddef.h>

#include "buf.h"

/* `event_name` is empty when the event has no `event:` field. Consecutive `data:` fields are joined
 * with newlines. Both strings are parser-owned and valid only during the callback. Returning
 * non-zero suppresses later callbacks from the stream. */
typedef int (*sse_cb)(const char *event_name, const char *data, void *user);

struct sse_parser {
    struct buf line;
    struct buf event;
    struct buf data;
    sse_cb callback;
    void *callback_user;
    int callback_stopped;
};

void sse_parser_init(struct sse_parser *parser, sse_cb callback, void *user);
void sse_parser_free(struct sse_parser *parser);

/* Partial lines are retained between calls. */
void sse_parser_feed(struct sse_parser *parser, const char *data, size_t len);

/* Emit an event buffered without a terminating blank line. */
void sse_parser_finalize(struct sse_parser *parser);

#endif /* HAX_TRANSPORT_SSE_H */
