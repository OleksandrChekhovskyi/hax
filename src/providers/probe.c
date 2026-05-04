/* SPDX-License-Identifier: MIT */
#include "probe.h"

#include <stdatomic.h>
#include <stdlib.h>

#include "bg.h"
#include "http.h"

/* Free everything we own and the args struct itself. Tolerant of
 * partially-populated args so the spawn-failure path can reuse it
 * without first having to mirror the worker's full setup. */
static void probe_args_free(struct probe_args *a)
{
    if (!a)
        return;
    free(a->url);
    if (a->headers) {
        for (char **h = a->headers; *h; h++)
            free(*h);
        free(a->headers);
    }
    if (a->user && a->free_user)
        a->free_user(a->user);
    free(a);
}

static void probe_run(struct bg_job *job, void *arg)
{
    (void)job;
    struct probe_args *a = arg;
    /* Cancellation between spawn and the first network byte: the bg
     * cancel hook short-circuits libcurl from the progress callback,
     * but we may also have been asked to quit before we even started.
     * Skip the round-trip in that case. */
    if (bg_cancel_thunk()) {
        probe_args_free(a);
        return;
    }

    char *body = NULL;
    int rc =
        http_get(a->url, (const char *const *)a->headers, a->timeout_s, bg_cancel_thunk, &body);
    if (rc == 0 && body) {
        long v = a->extract(body, a->user);
        if (v > 0)
            atomic_store(a->target, v);
    }
    free(body);
    probe_args_free(a);
}

struct bg_job *probe_context_limit_spawn(struct probe_args *args)
{
    if (!args)
        return NULL;
    struct bg_job *job = bg_spawn(probe_run, args);
    /* Worker frees args on its own exit path; on spawn failure that
     * path never runs, so we own the cleanup here. Keeps the caller
     * from having to know about the partially-built state. */
    if (!job)
        probe_args_free(args);
    return job;
}
