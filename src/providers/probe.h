/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROBE_H
#define HAX_PROBE_H

#include <stdatomic.h>

#include "bg.h"

/* Shared scaffolding for the "GET a small JSON catalog, find one
 * integer, store it in provider->context_limit" pattern. The catalog
 * shapes differ (codex's data.models[].context_window keyed by slug,
 * openrouter's data[].context_length keyed by id, llama.cpp's
 * default_generation_settings.n_ctx with no key) — only the JSON walk
 * varies, so each provider supplies a small `extract` callback while
 * this helper owns the bg-cancel wiring, http_get call, atomic store,
 * and ownership of the heap-allocated args / headers / user data.
 *
 * Workers run on a bg thread spawned via bg_spawn(probe_context_limit_run,
 * args). The caller hands ownership of `args` to the worker; the worker
 * always frees it (and everything it points to) before returning, even
 * when the request fails or is cancelled. */

struct probe_args {
    /* Heap-owned. */
    char *url;
    /* NULL-terminated array of "Key: value" strings. Each entry and the
     * array itself are heap-owned. NULL = no extra headers. */
    char **headers;
    /* Total request timeout in seconds, passed through to http_get.
     * Probes are best-effort, so callers should pick a small value
     * (typically 5s) — a slow catalog endpoint shouldn't keep the
     * probe thread alive indefinitely. */
    int timeout_s;
    /* Body parser. Returns the discovered context-window value (>0) on
     * success, or 0 to mean "not found / malformed / not applicable".
     * 0 results are silently dropped — the helper does not update
     * `target` in that case. The body buffer is NUL-terminated and
     * lives only for the duration of the call. */
    long (*extract)(const char *body, void *user);
    /* Opaque caller state for `extract`. Heap-owned when free_user is
     * non-NULL; otherwise treated as an unowned reference (use this
     * for static strings that don't need freeing). */
    void *user;
    void (*free_user)(void *);
    /* Slot to update on success. Typically &p->context_limit on the
     * provider that spawned this probe. The atomic must outlive the
     * probe — which is guaranteed by main.c joining p->probe before
     * destroy. */
    _Atomic long *target;
};

/* Take ownership of `args` and launch the probe on a bg thread. Returns
 * the bg_job handle on success (caller stores into provider->probe so
 * main.c can join it before destroy), or NULL when pthread_create
 * fails — in which case `args` is freed for you, so the caller never
 * has to track a partial-construction cleanup branch. */
struct bg_job *probe_context_limit_spawn(struct probe_args *args);

#endif /* HAX_PROBE_H */
