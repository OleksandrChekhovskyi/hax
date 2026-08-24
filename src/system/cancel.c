/* SPDX-License-Identifier: MIT */
#include "system/cancel.h"

#include <stdatomic.h>

static atomic_int pause_requested;
static atomic_int abort_requested;

void cancel_request_pause(void)
{
    if (atomic_exchange(&pause_requested, 1))
        atomic_store(&abort_requested, 1);
}

void cancel_request_abort(void)
{
    atomic_store(&pause_requested, 1);
    atomic_store(&abort_requested, 1);
}

int cancel_pause_requested(void)
{
    return atomic_load(&pause_requested);
}

int cancel_abort_requested(void)
{
    return atomic_load(&abort_requested);
}

void cancel_clear_requests(void)
{
    atomic_store(&abort_requested, 0);
    atomic_store(&pause_requested, 0);
}
