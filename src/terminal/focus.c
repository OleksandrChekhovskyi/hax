/* SPDX-License-Identifier: MIT */
#include "terminal/focus.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#include "terminal/ansi.h"

static atomic_int current_focus = FOCUS_IN;
static volatile sig_atomic_t reporting_enabled;

void focus_reporting_enable(void)
{
    fputs(ANSI_FOCUS_REPORTING_ENABLE, stdout);
    fflush(stdout);
    reporting_enabled = 1;
}

void focus_reporting_disable(void)
{
    reporting_enabled = 0;
    fputs(ANSI_FOCUS_REPORTING_DISABLE, stdout);
    fflush(stdout);
}

int focus_reporting_suspend(void)
{
    static const char disable[] = ANSI_FOCUS_REPORTING_DISABLE;
    if (!reporting_enabled)
        return 0;
    (void)!write(STDOUT_FILENO, disable, sizeof(disable) - 1);
    return 1;
}

void focus_reporting_resume(void)
{
    static const char enable[] = ANSI_FOCUS_REPORTING_ENABLE;
    (void)!write(STDOUT_FILENO, enable, sizeof(enable) - 1);
}

void focus_record(enum focus_state state)
{
    atomic_store(&current_focus, (int)state);
}

enum focus_state focus_current(void)
{
    return (enum focus_state)atomic_load(&current_focus);
}
