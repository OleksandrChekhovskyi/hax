/* SPDX-License-Identifier: MIT */
#include "render/render_ctx.h"

#include <stdio.h>

#include "render/markdown.h"
#include "render/spinner.h"
#include "terminal/ansi.h"
#include "util.h"

/* Move the live render state to `to`: close whatever is currently
 * open (SGR reset, md tail flush, state-owned spinner hidden), then
 * open the target (block separator, fresh SGR, canonical label).
 *
 * Spinner ordering invariant: the owning state's spinner is hidden in
 * its close-half, before any newline/separator writes — a parked
 * spinner's hide restores the cursor to the content position, and
 * writing first would strand the restore target. */
void render_transition(struct render_ctx *r, enum render_state to)
{
    if (r->state == to)
        return;

    /* A real state change while a table was still buffering (e.g. the
     * stream ended mid-table and the close-half md_flush is about to
     * finalize the grid): hide the composing spinner first so the grid
     * doesn't render into its row. Same-state RS_TEXT transitions during
     * buffering returned above — render_text_chunk owns the spinner there. */
    if (r->table_composing) {
        spinner_hide(r->spinner);
        r->table_composing = 0;
    }

    switch (r->state) {
    case RS_IDLE:
        break;
    case RS_WAITING:
        spinner_hide(r->spinner);
        break;
    case RS_REASONING:
        if (r->md)
            md_flush(r->md);
        disp_raw(ANSI_RESET);
        disp_putc(&r->disp, '\n');
        break;
    case RS_TEXT:
        if (r->md)
            md_flush(r->md);
        break;
    case RS_CLUSTER:
        /* Hide restores the cursor to the cluster's own position; a
         * still-open coalescing read line then needs its terminating
         * \n (bash quiet lines committed theirs when written). */
        spinner_hide(r->spinner);
        if (r->cluster_line_open)
            disp_putc(&r->disp, '\n');
        fflush(stdout);
        r->cluster_last_tool = NULL;
        r->cluster_line_open = 0;
        r->cluster_line_used = 0;
        break;
    }

    /* Label rule: whichever path makes the spinner visible states its
     * own ground truth with an immediate set — the WAITING and CLUSTER
     * opens here, dispatch's parks, the table/compact paths. That
     * bounds staleness at state changes (a settled label from a prior
     * state can't re-debut on a later wait) and can't flicker, since
     * the row is drawn fresh. States that keep the spinner hidden
     * (TEXT, REASONING) set nothing — a label that can't render is a
     * dead store. Deferred requests are for same-state churn only
     * (item seams, compose batches). */
    switch (to) {
    case RS_IDLE:
        /* No separator emitted — out-of-band block writers
         * (render_open_block, display_tool_header) own that decision. */
        break;
    case RS_WAITING:
        disp_block_separator(&r->disp);
        spinner_set_label(r->spinner, "working", "working...");
        spinner_show(r->spinner);
        break;
    case RS_REASONING:
        spinner_hide(r->spinner);
        disp_block_separator(&r->disp);
        disp_raw(ANSI_DIM ANSI_ITALIC);
        /* Suppress md's inline SGR so the dim+italic span above is
         * the only active style; wrap and block structure still run. */
        if (r->md)
            md_set_styled(r->md, 0);
        break;
    case RS_TEXT:
        spinner_hide(r->spinner);
        disp_block_separator(&r->disp);
        r->disp.saw_text = 1;
        if (r->md)
            md_set_styled(r->md, 1);
        break;
    case RS_CLUSTER:
        spinner_hide(r->spinner);
        disp_block_separator(&r->disp);
        /* Displayed on the parked row until a run:<tool> request
         * settles — must not inherit a stale composing label. */
        spinner_set_label(r->spinner, "working", "working...");
        break;
    }

    r->state = to;
}

/* Return to the busy full-size spinner between visible content blocks.
 * This closes text/reasoning state (flushing md tails and SGR) and
 * resets the table-stall clock so it can't fire against a block that
 * is already closed. */
static void render_stream_wait(struct render_ctx *r)
{
    r->last_text_at = 0;
    render_transition(r, RS_WAITING);
    /* Same-state transitions are no-ops; still request the neutral
     * label and ensure the spinner is up. A request, not an immediate
     * set — seams fire between every streamed item, and an instant
     * reset would flicker against the labels surrounding events
     * request. */
    spinner_request_label(r->spinner, "working", "working...");
    spinner_show(r->spinner);
}

void render_stream_seam(struct render_ctx *r)
{
    if (r->state == RS_CLUSTER) {
        spinner_request_label(r->spinner, "working", "working...");
        return;
    }
    render_stream_wait(r);
}

void render_stream_begin(struct render_ctx *r)
{
    r->last_text_at = 0;
    r->retry_deadline_at = 0;
    if (r->state == RS_CLUSTER)
        return;
    render_stream_wait(r);
}

void render_open_block(struct render_ctx *r)
{
    render_transition(r, RS_IDLE);
    /* Belt-and-braces: RS_WAITING owns the busy spinner and is closed
     * by the transition above, but hide explicitly so callers that enter
     * with an out-of-band spinner row never draw into a line that a later
     * erase would wipe. */
    spinner_hide(r->spinner);
    disp_block_separator(&r->disp);
}

void render_table_spinner_show(struct render_ctx *r)
{
    if (r->table_composing)
        return;
    /* Only over the answer stream: RS_REASONING's dim+italic span is
     * emitted once on entry, and the spinner's trailing ANSI_RESET
     * would close it and leave the rest of the block unstyled. */
    if (r->state != RS_TEXT)
        return;
    /* Drain held newlines so the \r + erase-line lands on a fresh row
     * instead of wiping prior text. Immediate label set — this path
     * already did its own settling via the table stall clock. */
    disp_emit_held(&r->disp);
    spinner_set_label(r->spinner, "composing", "composing...");
    spinner_show(r->spinner);
    r->table_composing = 1;
}

void render_text_chunk(struct render_ctx *r, const char *s, size_t n)
{
    /* The table-composing spinner sits on its own row. Hide it before
     * feeding: this delta may finalize the table (md_feed emits the grid,
     * which must not race the spinner row) or buffer another row. */
    int had_spinner = r->table_composing;
    if (had_spinner) {
        spinner_hide(r->spinner);
        r->table_composing = 0;
    }
    int was_in_table = r->md && md_in_table(r->md);
    if (r->md)
        md_feed(r->md, s, n);
    else
        disp_write(&r->disp, s, n);
    fflush(stdout);
    if (r->md && md_in_table(r->md)) {
        /* Still buffering: nothing more shows until the grid lays out,
         * so leave the stall clock pinned and let the tick surface the
         * spinner past the threshold. The delta that *enters* the
         * table restarts the clock — its freshly rendered prose (or
         * the table's start) is the right zero point; a stale
         * last_text_at from before a pause would trip the tick
         * immediately. Re-show synchronously if we just hid one — a
         * repaint, not a flicker. */
        if (!was_in_table)
            r->last_text_at = monotonic_ms();
        if (had_spinner)
            render_table_spinner_show(r);
    } else {
        r->last_text_at = monotonic_ms();
    }
}

void render_text_delta(struct render_ctx *r, const char *s, size_t n)
{
    disp_first_delta_strip(&r->disp, &s, &n);
    if (n == 0)
        return;
    render_transition(r, RS_TEXT);
    render_text_chunk(r, s, n);
}

void update_retry_label(struct render_ctx *r)
{
    long remaining = r->retry_deadline_at - monotonic_ms();
    /* Round up so the countdown shows "1s" through the final second
     * instead of dropping to "0s" with time still on the clock. Floor
     * at 1s when we're inside the last 100ms — saying "in 0s" reads as
     * "stuck", and the actual flip to a fresh attempt is imminent. */
    long secs = (remaining + 999) / 1000;
    if (secs < 1)
        secs = 1;
    char buf[64];
    snprintf(buf, sizeof(buf), "retrying in %lds (attempt %d/%d)...", secs, r->retry_attempt,
             r->retry_max);
    spinner_set_label(r->spinner, "retry", buf);
}
