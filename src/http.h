/* SPDX-License-Identifier: MIT */
#ifndef HAX_HTTP_H
#define HAX_HTTP_H

#include <stddef.h>

#include "sse.h"

struct http_response {
    long status;
    char *error_body; /* non-null on non-2xx or transport error; caller frees */
};

/* headers: NULL-terminated array of "Key: Value" strings. */
int http_sse_post(const char *url, const char *const *headers, const char *body, size_t body_len,
                  sse_cb cb, void *user, struct http_response *resp);

#endif /* HAX_HTTP_H */
