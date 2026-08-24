/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_WIN32_INCLUDE_SCHED_H
#define HAX_SYSTEM_WIN32_INCLUDE_SCHED_H

#include <windows.h>

static inline int sched_yield(void)
{
    Sleep(0);
    return 0;
}

#endif /* HAX_SYSTEM_WIN32_INCLUDE_SCHED_H */
