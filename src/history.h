/* SPDX-License-Identifier: MIT */
#ifndef HAX_HISTORY_H
#define HAX_HISTORY_H

#include <stddef.h>

#include "render/render_ctx.h"

struct item;

/* Render stored conversation items the way the REPL rendered them live:
 * the accent-striped user prompt, markdown-rendered assistant text,
 * reasoning when it is being shown, and tool calls in their bracketed
 * idiom. The sibling of transcript.{c,h} — that one shows the
 * conversation as the *model* sees it (system prompt, tool schemas, every
 * argument and result verbatim); this one shows it as the *user* saw it.
 *
 * Everything goes through the live render pipeline (render_ctx → disp →
 * markdown), so a rendered turn is byte-identical to the original. That
 * pipeline paints with the cursor, so a caller writing anywhere but a
 * terminal (a pager, a file) renders into a memory stream and passes the
 * bytes through vt_resolve first.
 *
 * Not shown, deliberately: turn boundaries and per-turn usage footers
 * (the transcript and /session own accounting), and result images —
 * `read` is the only tool that attaches them and it renders silent, so
 * they never had a preview to reproduce. */

enum history_detail {
    /* Every tool call collapses to one dim "[name] arg" line and results
     * are omitted. For the inline resume replay, where the whole budget
     * is the screen the user is about to type on. */
    HISTORY_BRIEF,
    /* Verbose calls keep their header and their output preview, rebuilt
     * from the stored result through the same renderer the live preview
     * used. Silent calls stay one-liners — they showed no output live
     * either. For the paged view, where length is free. */
    HISTORY_FULL,
};

/* Render items[start_idx .. n_items) into `r`. The caller owns framing
 * (the dim rule above a replay, block separation on either side) and
 * leaves the render state at RS_IDLE afterwards if it needs a clean line;
 * `r->show_reasoning` decides whether reasoning items render, and
 * `r->md` being NULL renders text unwrapped, exactly as live. */
void history_render(struct render_ctx *r, enum history_detail detail, const struct item *items,
                    size_t n_items, size_t start_idx);

#endif /* HAX_HISTORY_H */
