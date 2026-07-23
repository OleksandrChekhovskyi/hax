/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROBE_H
#define HAX_PROBE_H

#include <stdatomic.h>

#include "system/bg_job.h"

/* Shared scaffolding for the "GET a small JSON catalog, pull a few
 * integers out of it, store them in provider atomics" pattern. The
 * catalog shapes differ (codex's data.models[].context_window keyed by
 * slug, openrouter's per-model architecture + endpoints, llama.cpp's
 * /props) — only the JSON walks vary, so each provider supplies small
 * `extract` callbacks while this helper owns the bg-cancel wiring, the
 * single http round-trip, the atomic stores, and ownership of the
 * heap-allocated args / headers / user data.
 *
 * Workers run on a bg thread spawned via bg_job_spawn(probe_run, args).
 * The caller hands ownership of `args` to the worker; the worker always
 * frees it (and everything it points to) before returning, even when the
 * request fails or is cancelled.
 *
 * Scalars only, by design: values land in _Atomic longs precisely so the
 * worker can publish while the foreground reads on every render with no
 * locking or lifetime questions. Enums, bitmasks, counts, and sizes all
 * fit. If a probe ever needs a string or a struct, that's the signal to
 * reach for a different mechanism (the catalog's cache-file + memoized
 * parse pattern), not to widen this one. */

/* One value to pull from the response body. `extract` returns the
 * discovered value (>0) on success, or 0 for "not found / malformed /
 * not applicable" — 0 results are silently dropped and `target` is left
 * untouched. The body buffer is NUL-terminated and lives only for the
 * duration of the call. A NULL `extract` marks the slot unused (callers
 * assign fields positionally and may skip one, e.g. when a user override
 * makes the context-limit extraction unwanted). */
struct probe_field {
    long (*extract)(const char *body, void *user);
    _Atomic long *target;
};

#define PROBE_FIELDS_MAX 4

struct probe_args {
    /* Heap-owned. */
    char *url;
    /* NULL-terminated array of "Key: value" strings. Each entry and the
     * array itself are heap-owned. NULL = no extra headers. */
    char **headers;
    /* Optional heap-owned JSON request body. When non-NULL the worker
     * issues a POST (Content-Type: application/json appended for free)
     * instead of a GET — used by probes whose catalog endpoint takes a
     * body (ollama's /api/show) rather than a query in the URL. */
    char *body;
    /* Total request timeout in seconds, passed through to http_get.
     * Probes are best-effort, so callers should pick a small value
     * (typically 5s) — a slow catalog endpoint shouldn't keep the
     * probe thread alive indefinitely. */
    int timeout_s;
    /* Opaque caller state shared by every extract. Heap-owned when
     * free_user is non-NULL; otherwise treated as an unowned reference
     * (use this for static strings that don't need freeing). */
    void *user;
    void (*free_user)(void *);
    /* Values to extract — every non-NULL slot's extract runs over the
     * same response body. The atomics must outlive the probe, which is
     * guaranteed by main.c joining the provider's probe handle before
     * destroy. */
    struct probe_field fields[PROBE_FIELDS_MAX];
};

/* Take ownership of `args` and launch the probe on a bg thread. Returns
 * the bg_job handle on success (caller stores into provider->probe so
 * main.c can join it before destroy), or NULL when pthread_create
 * fails — in which case `args` is freed for you, so the caller never
 * has to track a partial-construction cleanup branch. */
struct bg_job *probe_spawn(struct probe_args *args);

#endif /* HAX_PROBE_H */
