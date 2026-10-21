/* SPDX-License-Identifier: MIT */
#include "busy.h"

#include <stdio.h>
#include <stdlib.h>

#include "agent_core.h"
#include "xalloc.h"
#include "render/footer.h"
#include "terminal/ansi.h"
#include "terminal/interrupt.h"

struct busy {
    struct footer *footer;
};

struct busy *busy_begin(const char *label)
{
    struct busy *b = xmalloc(sizeof(*b));
    b->footer = footer_new(label);
    footer_show_deferred(b->footer);
    interrupt_clear_requests();
    interrupt_arm();
    return b;
}

int busy_tick(void *user)
{
    (void)user;
    return interrupt_abort_requested();
}

int busy_end(struct busy *b)
{
    interrupt_resolve_pending_escape();
    int cancelled = interrupt_abort_requested();
    interrupt_disarm();
    footer_hide(b->footer);
    footer_free(b->footer);
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
