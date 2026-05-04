/* SPDX-License-Identifier: MIT */
#include "bg.h"

#include <stdatomic.h>
#include <time.h>

#include "harness.h"

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
     * If the test deadlocks here, bg_cancel isn't reaching the worker. */
    while (!bg_cancelled(job))
        sleep_ms(1);
    atomic_store(&c->v, 99);
}

static void thunk_worker(struct bg_job *job, void *arg)
{
    (void)job;
    struct counter *c = arg;
    /* bg_cancel_thunk reads thread-local state populated by the bg
     * trampoline — mirrors how a probe inside http_get's progress
     * callback would observe a cancel. */
    while (!bg_cancel_thunk())
        sleep_ms(1);
    atomic_store(&c->v, 7);
}

int main(void)
{
    /* Basic spawn / run / join. */
    struct counter c1 = {0};
    struct bg_job *j1 = bg_spawn(inc_worker, &c1);
    EXPECT(j1 != NULL);
    bg_join(j1);
    EXPECT(atomic_load(&c1.v) == 42);

    /* Cancel signal observed by bg_cancelled(). */
    struct counter c2 = {0};
    struct bg_job *j2 = bg_spawn(cancel_loop_worker, &c2);
    EXPECT(j2 != NULL);
    /* Give the worker a moment to enter its loop before signalling. */
    sleep_ms(5);
    bg_cancel(j2);
    bg_join(j2);
    EXPECT(atomic_load(&c2.v) == 99);

    /* Same signal reachable through the parameterless thunk that
     * workers pass to http_get. */
    struct counter c3 = {0};
    struct bg_job *j3 = bg_spawn(thunk_worker, &c3);
    EXPECT(j3 != NULL);
    sleep_ms(5);
    bg_cancel(j3);
    bg_join(j3);
    EXPECT(atomic_load(&c3.v) == 7);

    /* NULL is a no-op for cancel/join — guards against awkward
     * call-sites that hold a probe handle that may be NULL. */
    bg_cancel(NULL);
    bg_join(NULL);
    EXPECT(bg_cancelled(NULL) == 0);

    /* Outside any bg context the thunk reads NULL TLS and reports not
     * cancelled. */
    EXPECT(bg_cancel_thunk() == 0);

    T_REPORT();
}
