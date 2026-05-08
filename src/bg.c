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

static void *bg_trampoline(void *p)
{
    struct bg_job *job = p;
    job->fn(job, job->arg);
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

int bg_tick(void *job)
{
    return bg_cancelled(job);
}

void bg_join(struct bg_job *job)
{
    if (!job)
        return;
    pthread_join(job->tid, NULL);
    free(job);
}
