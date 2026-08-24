/* SPDX-License-Identifier: MIT */
#include "harness.h"
#include "system/cancel.h"

static void test_starts_clear(void)
{
    cancel_clear_requests();
    EXPECT(!cancel_pause_requested());
    EXPECT(!cancel_abort_requested());
}

static void test_first_pause_request_does_not_abort(void)
{
    cancel_clear_requests();
    cancel_request_pause();
    EXPECT(cancel_pause_requested());
    EXPECT(!cancel_abort_requested());
}

static void test_repeated_pause_request_escalates(void)
{
    cancel_clear_requests();
    cancel_request_pause();
    cancel_request_pause();
    EXPECT(cancel_pause_requested());
    EXPECT(cancel_abort_requested());
}

static void test_abort_implies_pause(void)
{
    cancel_clear_requests();
    cancel_request_abort();
    EXPECT(cancel_abort_requested());
    EXPECT(cancel_pause_requested());
}

static void test_requests_stay_latched(void)
{
    cancel_clear_requests();
    cancel_request_abort();
    for (int i = 0; i < 3; i++) {
        EXPECT(cancel_abort_requested());
        EXPECT(cancel_pause_requested());
    }
    cancel_clear_requests();
    EXPECT(!cancel_abort_requested());
    EXPECT(!cancel_pause_requested());
}

int main(void)
{
    test_starts_clear();
    test_first_pause_request_does_not_abort();
    test_repeated_pause_request_escalates();
    test_abort_implies_pause();
    test_requests_stay_latched();
    T_REPORT();
}
