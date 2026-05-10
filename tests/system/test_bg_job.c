/* SPDX-License-Identifier: MIT */
#include <stdatomic.h>
#include <time.h>

#include "harness.h"
#include "system/bg_job.h"

/* usleep is removed by POSIX 2008 (which the project sets); use
 * nanosleep with whole millisecond input. Not signal-safe but tests
 * don't need to be. */
static void sleep_ms(long ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

struct counter {
    _Atomic int v;
};

static void inc_worker(struct bg_job *job, void *arg)
{
    (void)job;
    struct counter *c = arg;
    atomic_store(&c->v, 42);
}

static void cancel_loop_worker(struct bg_job *job, void *arg)
{
    struct counter *c = arg;
    /* Spin until cancelled, with a tiny sleep so we don't burn the CPU.
     * If the test deadlocks here, bg_job_cancel isn't reaching the worker. */
    while (!bg_job_cancelled(job))
        sleep_ms(1);
    atomic_store(&c->v, 99);
}

static void tick_worker(struct bg_job *job, void *arg)
{
    struct counter *c = arg;
    /* bg_job_tick is the http_tick_cb-shaped wrapper a probe inside
     * http_get's progress callback would invoke — same job pointer
     * as the worker holds. */
    while (!bg_job_tick(job))
        sleep_ms(1);
    atomic_store(&c->v, 7);
}

int main(void)
{
    /* Basic spawn / run / join. */
    struct counter c1 = {0};
    struct bg_job *j1 = bg_job_spawn(inc_worker, &c1);
    EXPECT(j1 != NULL);
    bg_job_join(j1);
    EXPECT(atomic_load(&c1.v) == 42);

    /* Cancel signal observed by bg_job_cancelled(). */
    struct counter c2 = {0};
    struct bg_job *j2 = bg_job_spawn(cancel_loop_worker, &c2);
    EXPECT(j2 != NULL);
    /* Give the worker a moment to enter its loop before signalling. */
    sleep_ms(5);
    bg_job_cancel(j2);
    bg_job_join(j2);
    EXPECT(atomic_load(&c2.v) == 99);

    /* Same signal reachable through the http_tick_cb-shaped wrapper
     * that workers pass to http_get / http_sse_post. */
    struct counter c3 = {0};
    struct bg_job *j3 = bg_job_spawn(tick_worker, &c3);
    EXPECT(j3 != NULL);
    sleep_ms(5);
    bg_job_cancel(j3);
    bg_job_join(j3);
    EXPECT(atomic_load(&c3.v) == 7);

    /* NULL is a no-op for cancel/join — guards against awkward
     * call-sites that hold a probe handle that may be NULL. */
    bg_job_cancel(NULL);
    bg_job_join(NULL);
    EXPECT(bg_job_cancelled(NULL) == 0);

    /* NULL job is treated as not-cancelled by bg_job_tick too. */
    EXPECT(bg_job_tick(NULL) == 0);

    T_REPORT();
}
