/* SPDX-License-Identifier: MIT */
#ifndef HAX_SSE_H
#define HAX_SSE_H

#include <stddef.h>

#include "util.h"

/*
 * SSE callback. event_name is empty if no `event:` line was sent for this
 * event. data is the accumulated `data:` payload (lines joined by '\n').
 * Both strings are owned by the parser; do not retain past the callback.
 * Return non-zero to stop further callbacks for this stream.
 */
typedef int (*sse_cb)(const char *event_name, const char *data, void *user);

struct sse_parser {
    struct buf line;
    struct buf event;
    struct buf data;
    sse_cb cb;
    void *user;
    int cb_abort;
};

void sse_parser_init(struct sse_parser *p, sse_cb cb, void *user);
void sse_parser_free(struct sse_parser *p);

/* Feed a chunk of transport bytes. Boundary-safe: partial lines are buffered. */
void sse_parser_feed(struct sse_parser *p, const char *data, size_t n);

/* Flush any trailing line/event when the stream ends. */
void sse_parser_finalize(struct sse_parser *p);

#endif /* HAX_SSE_H */
