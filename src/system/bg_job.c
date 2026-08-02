/* SPDX-License-Identifier: MIT */
#include "system/bg_job.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#include "util.h"

struct bg_job {
    pthread_t thread;
    bg_job_fn fn;
    void *arg;
    atomic_bool cancel_requested;
};

static void *run_job(void *arg)
{
    struct bg_job *job = arg;
    job->fn(job, job->arg);
    return NULL;
}

struct bg_job *bg_job_spawn(bg_job_fn fn, void *arg)
{
    assert(fn);

    struct bg_job *job = xcalloc(1, sizeof(*job));
    job->fn = fn;
    job->arg = arg;
    atomic_init(&job->cancel_requested, false);
    if (pthread_create(&job->thread, NULL, run_job, job) != 0) {
        free(job);
        return NULL;
    }
    return job;
}

void bg_job_cancel(struct bg_job *job)
{
    if (job)
        atomic_store(&job->cancel_requested, true);
}

int bg_job_cancel_requested(const struct bg_job *job)
{
    return job && atomic_load(&job->cancel_requested);
}

int bg_job_cancel_tick(void *job)
{
    return bg_job_cancel_requested(job);
}

void bg_job_join(struct bg_job *job)
{
    if (!job)
        return;
    pthread_join(job->thread, NULL);
    free(job);
}
