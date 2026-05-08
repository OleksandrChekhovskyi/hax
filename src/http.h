/* SPDX-License-Identifier: MIT */
#ifndef HAX_HTTP_H
#define HAX_HTTP_H

#include <stddef.h>

#include "sse.h"

struct http_response {
    long status;
    char *error_body; /* non-null on non-2xx or transport error; caller frees */
    int cancelled;    /* 1 when transfer was aborted by tick returning non-zero */
};

/* Periodic side-channel callback. Called from libcurl's progress hook
 * (~1Hz, fires even when the server is silent) and on every received
 * chunk. Returning non-zero aborts the in-flight transfer promptly.
 * Side effects are allowed — the agent uses this slot to do wall-clock
 * idle detection ("model went quiet mid-text, surface a spinner")
 * alongside the cancel check. NULL = no tick (no cancel, no idle). */
typedef int (*http_tick_cb)(void *user);

/* headers: NULL-terminated array of "Key: Value" strings. */
int http_sse_post(const char *url, const char *const *headers, const char *body, size_t body_len,
                  sse_cb cb, void *user, http_tick_cb tick, void *tick_user,
                  struct http_response *resp);

/* Synchronous GET into a freshly-allocated NUL-terminated buffer. Used for
 * small JSON probes (e.g. /v1/models, /props) where streaming is overkill.
 *
 * Returns 0 on 2xx with *out set to a heap-owned response body (caller
 * frees). Returns -1 on any failure — transport, non-2xx, empty body — with
 * *out=NULL; the caller decides whether to surface or ignore the failure.
 *
 * `headers` is an optional NULL-terminated array of "Key: Value" strings,
 * may be NULL. `timeout_s` is the total request timeout in seconds; pass 0
 * to disable. Connect timeout is fixed at a short value so an unreachable
 * host fails fast.
 *
 * `tick` is an optional side-channel hook (same shape as for
 * http_sse_post). Background probes pass `bg_tick` with their job
 * pointer so shutdown can abort an in-flight transfer in well under a
 * second instead of waiting out the timeout. NULL = no tick. */
int http_get(const char *url, const char *const *headers, long timeout_s, http_tick_cb tick,
             void *tick_user, char **out);

#endif /* HAX_HTTP_H */
