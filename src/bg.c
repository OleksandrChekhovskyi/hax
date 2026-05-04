/* SPDX-License-Identifier: MIT */
#include "bg.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "util.h"

struct bg_job {
    pthread_t tid;
    bg_fn fn;
    void *arg;
    _Atomic int cancelled;
};

/* Per-thread pointer to the currently-running job, set by the trampoline
 * before the worker enters fn() and cleared after it returns. Lets the
 * cancel-hook adapter (bg_cancel_thunk) be a parameterless function so
 * workers can pass it directly to http_get / http_sse_post. */
static _Thread_local struct bg_job *bg_current;

static void *bg_trampoline(void *p)
{
    struct bg_job *job = p;
    bg_current = job;
    job->fn(job, job->arg);
    bg_current = NULL;
    return NULL;
}

struct bg_job *bg_spawn(bg_fn fn, void *arg)
{
    struct bg_job *job = xcalloc(1, sizeof(*job));
    job->fn = fn;
    job->arg = arg;
    if (pthread_create(&job->tid, NULL, bg_trampoline, job) != 0) {
        free(job);
        return NULL;
    }
    return job;
}

void bg_cancel(struct bg_job *job)
{
    if (job)
        atomic_store(&job->cancelled, 1);
}

int bg_cancelled(const struct bg_job *job)
{
    return job ? atomic_load(&job->cancelled) : 0;
}

int bg_cancel_thunk(void)
{
    return bg_cancelled(bg_current);
}

void bg_join(struct bg_job *job)
{
    if (!job)
        return;
    pthread_join(job->tid, NULL);
    free(job);
}
