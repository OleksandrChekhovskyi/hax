/* SPDX-License-Identifier: MIT */
#ifndef HAX_HTTP_H
#define HAX_HTTP_H

#include <stddef.h>

#include "sse.h"

struct http_response {
    long status;
    char *error_body; /* non-null on non-2xx or transport error; caller frees */
    int cancelled;    /* 1 when transfer was aborted by cancel_cb returning non-zero */
};

/* Optional polled-cancellation callback. When provided and it returns
 * non-zero, the in-flight transfer is aborted promptly:
 *   - on every received chunk (write path), and
 *   - periodically (~1Hz) via libcurl's progress callback, so a silent
 *     server can still be aborted without waiting for the next byte.
 * NULL = no cancellation. */
typedef int (*http_cancel_cb)(void);

/* headers: NULL-terminated array of "Key: Value" strings. */
int http_sse_post(const char *url, const char *const *headers, const char *body, size_t body_len,
                  sse_cb cb, void *user, http_cancel_cb cancel, struct http_response *resp);

#endif /* HAX_HTTP_H */
