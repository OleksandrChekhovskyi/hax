/* SPDX-License-Identifier: MIT */
#include <sched.h>
#include <stddef.h>

#include "harness.h"
#include "system/bg_job.h"

static void set_value(struct bg_job *job, void *arg)
{
    (void)job;
    *(int *)arg = 42;
}

static void wait_for_cancel(struct bg_job *job, void *arg)
{
    while (!bg_job_cancel_requested(job))
        sched_yield();
    *(int *)arg = 1;
}

static void wait_for_cancel_tick(struct bg_job *job, void *arg)
{
    while (!bg_job_cancel_tick(job))
        sched_yield();
    *(int *)arg = 1;
}

static void test_worker_runs(void)
{
    int value = 0;
    struct bg_job *job = bg_job_spawn(set_value, &value);

    EXPECT(job != NULL);
    bg_job_join(job);
    EXPECT(value == 42);
}

static void test_worker_observes_cancel_request(void)
{
    int cancelled = 0;
    struct bg_job *job = bg_job_spawn(wait_for_cancel, &cancelled);

    EXPECT(job != NULL);
    bg_job_cancel(job);
    bg_job_join(job);
    EXPECT(cancelled);
}

static void test_cancel_tick_observes_request(void)
{
    int cancelled = 0;
    struct bg_job *job = bg_job_spawn(wait_for_cancel_tick, &cancelled);

    EXPECT(job != NULL);
    bg_job_cancel(job);
    bg_job_join(job);
    EXPECT(cancelled);
}

static void test_null_job_is_inactive(void)
{
    bg_job_cancel(NULL);
    bg_job_join(NULL);
    EXPECT(!bg_job_cancel_requested(NULL));
    EXPECT(!bg_job_cancel_tick(NULL));
}

int main(void)
{
    test_worker_runs();
    test_worker_observes_cancel_request();
    test_cancel_tick_observes_request();
    test_null_job_is_inactive();
    T_REPORT();
}
