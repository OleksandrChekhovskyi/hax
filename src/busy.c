/* SPDX-License-Identifier: MIT */
#include "busy.h"

#include <stdio.h>
#include <stdlib.h>

#include "agent_core.h"
#include "util.h"
#include "render/spinner.h"
#include "system/cancel.h"
#include "terminal/ansi.h"
#include "terminal/interrupt.h"

struct busy {
    struct spinner *sp;
};

struct busy *busy_begin(const char *label)
{
    struct busy *b = xmalloc(sizeof(*b));
    b->sp = spinner_new(label);
    spinner_show(b->sp);
    cancel_clear_requests();
    interrupt_arm();
    return b;
}

int busy_tick(void *user)
{
    (void)user;
    return cancel_abort_requested();
}

int busy_end(struct busy *b)
{
    interrupt_resolve_pending_escape();
    int cancelled = cancel_abort_requested();
    interrupt_disarm();
    spinner_hide(b->sp);
    spinner_free(b->sp);
    free(b);
    /* A cancelled window would otherwise vanish without a trace — the
     * spinner is erased and the caller prints nothing. Leave the same
     * dim marker the agent loop leaves on an interrupted turn, so
     * scrollback shows the command was abandoned, not silently empty.
     * Only reachable on a TTY (cancelled is always 0 otherwise). */
    if (cancelled) {
        printf(ANSI_DIM INTERRUPT_MARKER ANSI_RESET "\n");
        fflush(stdout);
    }
    return cancelled;
}
