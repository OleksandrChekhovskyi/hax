/* SPDX-License-Identifier: MIT */
#include <time.h>

#include "harness.h"
#include "system/clock.h"

static void test_monotonic_ms_advances(void)
{
    long first = monotonic_ms();
    EXPECT(first >= 0);

    /* nanosleep() waits at least the requested time, so two elapsed milliseconds floor to a
     * difference of one or more. */
    struct timespec delay = {.tv_nsec = 2 * 1000 * 1000};
    nanosleep(&delay, NULL);

    long second = monotonic_ms();
    EXPECT(second - first >= 1);
}

int main(void)
{
    test_monotonic_ms_advances();

    T_REPORT();
}
