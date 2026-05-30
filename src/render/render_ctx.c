/* SPDX-License-Identifier: MIT */
#include "render/render_ctx.h"

#include <stdio.h>

#include "render/markdown.h"
#include "render/spinner.h"
#include "terminal/ansi.h"
#include "util.h"

/* Clear the idle inline overlay (SPINNER_INLINE_TEXT) if it's up. The
 * overlay is a "still alive" indicator drawn on top of an in-flight
 * content stream; any new render activity supersedes it. Called from
 * render_transition (covering both cross-state and same-state cases)
 * so handlers don't need to repeat it after every transition call. */
static void render_clear_idle(struct render_ctx *r)
{
    if (r->idle_shown) {
        spinner_hide(r->spinner);
        r->idle_shown = 0;
    }
}

/* Move the live render state to `to`: close whatever is currently
 * open (SGR reset, md tail flush, state-owned spinner hidden), then
 * open the target (block separator, fresh SGR, canonical label).
 *
 * Spinner ordering invariant: non-cluster spinners are hidden before
 * the open-half's block separator advances the cursor past the glyph
 * (which would make \b-space-\b erase impossible). The cluster's own
 * spinner is hidden inside the close-half of RS_CLUSTER where
 * was_inline drives correct NL/trail accounting. */
void render_transition(struct render_ctx *r, enum render_state to)
{
    render_clear_idle(r);
    if (r->state == to)
        return;

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
    case RS_CLUSTER: {
        int was_inline = spinner_hide(r->spinner);
        if (was_inline)
            disp_putc(&r->disp, '\n');
        else
            r->disp.trail = 1;
        fflush(stdout);
        r->cluster_last_tool = NULL;
        r->cluster_line_used = 0;
        break;
    }
    }

    switch (to) {
    case RS_IDLE:
        /* No separator emitted — out-of-band block writers
         * (render_open_block, display_tool_header) own that decision. */
        break;
    case RS_WAITING:
        disp_block_separator(&r->disp);
        spinner_set_label(r->spinner, "working...");
        spinner_show(r->spinner);
        break;
    case RS_REASONING:
        spinner_hide(r->spinner);
        disp_block_separator(&r->disp);
        disp_raw(ANSI_DIM ANSI_ITALIC);
        spinner_set_label(r->spinner, "thinking...");
        /* Suppress md's inline SGR so the dim+italic span above is
         * the only active style; wrap and block structure still run. */
        if (r->md)
            md_set_styled(r->md, 0);
        break;
    case RS_TEXT:
        spinner_hide(r->spinner);
        disp_block_separator(&r->disp);
        spinner_set_label(r->spinner, "working...");
        r->disp.saw_text = 1;
        if (r->md)
            md_set_styled(r->md, 1);
        break;
    case RS_CLUSTER:
        spinner_hide(r->spinner);
        disp_block_separator(&r->disp);
        break;
    }

    r->state = to;
}

/* Return to the busy full-size spinner between visible content blocks.
 * This closes text/reasoning state (flushing md tails and SGR) and clears
 * the text-idle window so a long tool-args / post-reasoning pause doesn't
 * render as an inline glyph attached to the prior block. */
static void render_stream_wait(struct render_ctx *r)
{
    r->last_text_at = 0;
    render_transition(r, RS_WAITING);
    /* Same-state RS_WAITING transitions are no-ops; still restore the
     * neutral label after invisible reasoning and ensure the spinner is up. */
    spinner_set_label(r->spinner, "working...");
    spinner_show(r->spinner);
}

void render_stream_seam(struct render_ctx *r)
{
    if (r->state == RS_CLUSTER) {
        spinner_set_label(r->spinner, "working...");
        return;
    }
    render_stream_wait(r);
}

void render_stream_begin(struct render_ctx *r)
{
    r->last_text_at = 0;
    r->idle_shown = 0;
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

void render_text_chunk(struct render_ctx *r, const char *s, size_t n)
{
    if (r->md)
        md_feed(r->md, s, n);
    else
        disp_write(&r->disp, s, n);
    fflush(stdout);
    r->last_text_at = monotonic_ms();
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
    spinner_set_label(r->spinner, buf);
}
