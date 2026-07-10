/* SPDX-License-Identifier: MIT */
#include "busy.h"

#include <stdio.h>
#include <stdlib.h>

#include "agent_core.h"
#include "util.h"
#include "render/spinner.h"
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
    interrupt_clear();
    interrupt_arm();
    return b;
}

int busy_tick(void *user)
{
    (void)user;
    return interrupt_requested();
}

/* Same settle → read → disarm sequence as the agent's streaming paths:
 * settle first so a just-typed \x1b that is still being classified
 * doesn't slip past the read. */
int busy_end(struct busy *b)
{
    interrupt_settle();
    int cancelled = interrupt_requested();
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
