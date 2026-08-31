/* SPDX-License-Identifier: MIT */
#include "system/bg_job.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#include "xalloc.h"

struct bg_job {
    pthread_t thread;
    bg_job_fn fn;
    void *arg;
    atomic_bool cancel_requested;
    pthread_mutex_t done_lock;
    pthread_cond_t done_cond;
    int done;
};

static void *run_job(void *arg)
{
    struct bg_job *job = arg;
    job->fn(job, job->arg);
    pthread_mutex_lock(&job->done_lock);
    job->done = 1;
    pthread_cond_signal(&job->done_cond);
    pthread_mutex_unlock(&job->done_lock);
    return NULL;
}

struct bg_job *bg_job_spawn(bg_job_fn fn, void *arg)
{
    assert(fn);

    struct bg_job *job = xcalloc(1, sizeof(*job));
    job->fn = fn;
    job->arg = arg;
    atomic_init(&job->cancel_requested, false);
    pthread_mutex_init(&job->done_lock, NULL);
    pthread_cond_init(&job->done_cond, NULL);
    if (pthread_create(&job->thread, NULL, run_job, job) != 0) {
        pthread_cond_destroy(&job->done_cond);
        pthread_mutex_destroy(&job->done_lock);
        free(job);
        return NULL;
    }
    return job;
}

int bg_job_wait_ms(struct bg_job *job, long timeout_ms)
{
    if (!job)
        return 1;

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&job->done_lock);
    /* Any nonzero result — timeout included — ends the wait; only 0 can be a spurious wakeup. */
    int rc = 0;
    while (!job->done && rc == 0)
        rc = pthread_cond_timedwait(&job->done_cond, &job->done_lock, &deadline);
    int done = job->done;
    pthread_mutex_unlock(&job->done_lock);
    return done;
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
    pthread_cond_destroy(&job->done_cond);
    pthread_mutex_destroy(&job->done_lock);
    free(job);
}
