/* SPDX-License-Identifier: MIT */
#ifndef HAX_HTTP_H
#define HAX_HTTP_H

#include <stddef.h>

#include "transport/sse.h"

struct http_response {
    long status;
    char *error_body;    /* non-null on non-2xx or transport error; caller frees */
    long retry_after_ms; /* parsed Retry-After header, 0 when absent/unparseable */
    int cancelled;       /* 1 when transfer was aborted by tick returning non-zero */
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
 * http_sse_post). Background probes pass `bg_job_tick` with their job
 * pointer so shutdown can abort an in-flight transfer in well under a
 * second instead of waiting out the timeout. NULL = no tick.
 *
 * `status_out` is an optional slot for the HTTP response code (0 when the
 * request never got a response, e.g. a transport error). It is filled on
 * both success and failure, letting callers distinguish a 401 from a
 * network blip even though the return code collapses all failures to -1.
 * NULL = caller doesn't need the status. */
int http_get(const char *url, const char *const *headers, long timeout_s, http_tick_cb tick,
             void *tick_user, char **out, long *status_out);

/* Synchronous POST of a JSON body into a freshly-allocated NUL-terminated
 * buffer. Identical contract to http_get otherwise (timeouts, tick, return
 * codes, *out ownership). `body` is treated as opaque bytes — the caller
 * is responsible for the JSON encoding — and a `Content-Type: application/json`
 * header is appended automatically (callers should not duplicate it in
 * `headers`). NULL/empty body sends a zero-length POST. Used for small
 * probe endpoints that take a JSON request (ollama's POST /api/show, …)
 * rather than encoding the query in the URL. */
int http_post_json(const char *url, const char *const *headers, const char *body, size_t body_len,
                   long timeout_s, http_tick_cb tick, void *tick_user, char **out);

struct http_fetch_result {
    long status;        /* HTTP status, or 0 on transport failure */
    char *body;         /* heap, NUL-terminated; caller frees. NULL on transport failure */
    size_t body_len;    /* byte length of body (excludes terminator) */
    char *content_type; /* heap, lowercased mime sans params, or NULL; caller frees */
    char *final_url;    /* heap, effective URL after redirects, or NULL; caller frees */
    char *error;        /* heap, transport error string, or NULL; caller frees */
    int truncated;      /* 1 if the download was stopped at max_bytes */
};

/* Fetch a URL's content for the web_fetch tool. Unlike http_get (a JSON-probe
 * helper) this follows redirects (capped, http/https only), sends a plain
 * User-Agent, hard-caps the downloaded body at `max_bytes`, and reports the
 * response Content-Type and post-redirect URL.
 *
 * Returns 0 whenever an HTTP exchange completed — including non-2xx, which the
 * caller inspects via `out->status` — with `out->body` holding whatever the
 * server sent (possibly truncated). Returns -1 on a transport failure (DNS,
 * connect, timeout, cancel) with `out->error` set and `out->body` NULL.
 *
 * `max_bytes` 0 means no download cap. `timeout_s` 0 disables the total
 * timeout. `tick` is the usual optional cancel/idle side channel. The caller
 * frees every non-NULL heap field in `out` (a single http_fetch_free helper
 * does this). */
int http_fetch(const char *url, size_t max_bytes, long timeout_s, http_tick_cb tick,
               void *tick_user, struct http_fetch_result *out);

/* Free every heap field of an http_fetch_result (safe on a zeroed struct). */
void http_fetch_free(struct http_fetch_result *r);

#endif /* HAX_HTTP_H */
