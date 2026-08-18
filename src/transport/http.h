/* SPDX-License-Identifier: MIT */
#ifndef HAX_TRANSPORT_HTTP_H
#define HAX_TRANSPORT_HTTP_H

#include <stddef.h>

#include "transport/sse.h"

struct http_response {
    long status;
    char *error_body;    /* heap-owned; NULL on success or cancellation */
    long retry_after_ms; /* 0 when Retry-After is absent or invalid */
    int cancelled;
};

/* Called during transfer progress and after each received chunk. Returning non-zero aborts. */
typedef int (*http_tick_cb)(void *user);

/* `headers` is a NULL-terminated array and may be NULL. `idle_timeout_s == 0` disables the
 * low-speed timeout. The response is initialized on every call; the caller owns `error_body`.
 * Returns 0 when the transfer completes, including non-2xx responses, and -1 otherwise. */
int http_sse_post(const char *url, const char *const *headers, const char *body, size_t body_len,
                  long idle_timeout_s, sse_cb cb, void *user, http_tick_cb tick, void *tick_user,
                  struct http_response *response);

/* Synchronous GET into a heap-owned NUL-terminated buffer. `headers` is a NULL-terminated array and
 * may be NULL. Returns 0 on a non-empty 2xx response; otherwise returns -1 and sets `*out` to NULL.
 * `timeout_s == 0` and `max_bytes == 0` disable their respective limits. `status_out`, when
 * non-NULL, receives 0 if no HTTP response was received. */
int http_get(const char *url, const char *const *headers, long timeout_s, long max_bytes,
             http_tick_cb tick, void *tick_user, char **out, long *status_out);

/* Synchronous JSON POST with the same response, timeout, and size contracts as http_get.
 * Content-Type is added automatically. A NULL body sends an empty POST. */
int http_post_json(const char *url, const char *const *headers, const char *body, size_t body_len,
                   long timeout_s, long max_bytes, http_tick_cb tick, void *tick_user, char **out);

/* Synchronous POST with a caller-supplied Content-Type value; NULL adds no Content-Type header.
 * Unlike http_post_json, a completed exchange returns 0 for every HTTP status: `*out` receives the
 * response body (NULL when empty) and `*status_out` the status, so callers can classify
 * protocol-level error payloads. Returns -1 only on transport failure or cancellation, with
 * `*status_out` set to 0. */
int http_post(const char *url, const char *const *headers, const char *content_type,
              const char *body, size_t body_len, long timeout_s, long max_bytes, http_tick_cb tick,
              void *tick_user, char **out, long *status_out);

#endif /* HAX_TRANSPORT_HTTP_H */
