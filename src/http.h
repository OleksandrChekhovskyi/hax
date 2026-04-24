/* SPDX-License-Identifier: MIT */
#ifndef HAX_HTTP_H
#define HAX_HTTP_H

#include <stddef.h>

/*
 * SSE callback. event_name is empty if no `event:` line was sent for this event.
 * data is the accumulated `data:` payload (lines joined by '\n').
 * Return non-zero to stop further events (transfer still drains).
 */
typedef int (*sse_cb)(const char *event_name, const char *data, void *user);

struct http_response {
    long status;
    char *error_body; /* non-null on non-2xx or transport error; caller frees */
};

/* headers: NULL-terminated array of "Key: Value" strings. */
int http_sse_post(const char *url, const char *const *headers, const char *body, size_t body_len,
                  sse_cb cb, void *user, struct http_response *resp);

#endif /* HAX_HTTP_H */
