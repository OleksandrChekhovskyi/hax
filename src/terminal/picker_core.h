/* SPDX-License-Identifier: MIT */
#ifndef HAX_PICKER_CORE_H
#define HAX_PICKER_CORE_H

#include <stddef.h>
#include <termios.h>

#include "util.h" /* struct buf */
#include "terminal/picker.h"

/*
 * Pure (no-IO) picker filter and selection/scroll state machine, exposed
 * for testing. `picker.c` owns the tty/render layer and drives these ops
 * in response to decoded keypresses; tests can drive them directly without
 * a tty by building a state over a synthetic item list.
 *
 * `struct picker_state` is the shared state between the two layers — its
 * render/tty fields (painted, prev_rows, saved, raw_active, query) are
 * touched only by picker.c and stay zero / unused in headless test
 * contexts, mirroring how input_core.h exposes struct input.
 */

/* Width of the leading marker column: "→ " on the highlighted row, spaces
 * elsewhere. The search field's icon is the same width so the query and the
 * row labels align vertically. */
#define PICKER_MARKER_CELLS 2

/* Ceiling on the rows one painted frame can hold: title and its blank, the
 * search field and its blank, the list window, the footer and its blank.
 * Generous — it only bounds the recorded row widths below. */
#define PICKER_FRAME_ROWS_MAX 32

struct picker_state {
    const struct picker_opts *opts;
    size_t *filtered; /* item indices currently matching the filter */
    size_t n_filtered;
    size_t sel; /* offset into filtered[] of the highlighted row */
    size_t top; /* offset into filtered[] of the first visible row */
    int viewport;
    /* Footer height, held constant across selections so the frame doesn't
     * jump; 0 when nothing can appear there. Recomputed on resize. */
    int footer_lines;
    struct buf query;

    int painted;   /* a paint has happened (so we can reposition over it) */
    int prev_rows; /* rows the last paint emitted (cursor sits on the last) */
    /* Cell width of each of those rows. A row is painted one per screen
     * row, but a later width change makes the terminal reflow it across
     * several — so repositioning over the last frame needs its widths, not
     * just how many rows it had. */
    int prev_widths[PICKER_FRAME_ROWS_MAX];
    /* Terminal geometry the current viewport/footer_lines were computed
     * for; a repaint that finds different dimensions redoes that layout. */
    int cols;
    int rows;

    struct termios saved;
    int raw_active;
};

/* Display width of `s` as a picker row renders it: measured to the first
 * newline (rows are always one physical line), plus one cell for the
 * ellipsis that replaces the remainder when there is one. */
int picker_core_clip_width(const char *s);

/* Cells the row layout grants `it`'s label in a `cols`-wide terminal.
 *
 * A row is `label [ ✓ current ] [ detail ]`, and detail claims what the
 * label doesn't need — so a label can be clipped well before it reaches the
 * full row width. Shared with the render path rather than approximated,
 * because the footer has to decide *before the first paint* how many lines
 * to reserve for labels it may need to repeat (see picker_opts.label_gutter),
 * and a reservation that disagreed with the eventual clipping would leave a
 * clipped row with nowhere to show its full text. */
int picker_core_label_cells(const struct picker_item *it, int cols);

/* Append `n` bytes of `s` to `out`, substituting one '?' for every
 * codepoint utf8_codepoint_cells rejects — C0 controls (ESC included), the
 * Trojan Source bidi vectors, and the rest of the invisible-hazard set.
 *
 * Every byte of caller-supplied text must reach a painted frame through
 * this or picker.c's append_clip, which applies the same filter under a
 * cell budget. Item text is not the picker's own: a row can carry a model
 * description written by a third party and relayed by a provider catalog,
 * and the frame emitter deliberately passes escape sequences through at
 * zero width so the picker's SGR codes survive — so a raw append would
 * hand that party the terminal. */
void picker_core_append_sanitized(struct buf *out, const char *s, size_t n);

/* Pure filter predicate: case-insensitive, `query` split on spaces into
 * terms, every term must occur as a substring of `text`. An empty or
 * NULL query matches everything. NULL `text` matches only an empty query. */
int picker_core_match(const char *text, const char *query);

/* Rebuild filtered[] from opts against the current query, resetting the
 * selection and window to the first match. */
void picker_core_recompute(struct picker_state *s);

/* Pull the selection and scroll window back into range. Called internally
 * after every move; exposed for the one external mutation of the geometry
 * they depend on — a terminal resize changes `viewport` under a window that
 * was valid for the old one. */
void picker_core_clamp_scroll(struct picker_state *s);

/* Step the selection one row in `delta`'s direction, stopping at the list
 * ends. */
void picker_core_move_sel(struct picker_state *s, int delta);

/* Jump the selection by half a viewport in `dir`'s direction, re-centering
 * the window. */
void picker_core_page_sel(struct picker_state *s, int dir);

/* Jump to the first / last match. */
void picker_core_select_first(struct picker_state *s);
void picker_core_select_last(struct picker_state *s);

/* Park the selection on the filtered row showing item `item_idx`, centering
 * the window on it. No-op when the item isn't in the current filter. Used
 * to open the picker with the cursor on the caller's `initial` row. */
void picker_core_select_item(struct picker_state *s, size_t item_idx);

#endif /* HAX_PICKER_CORE_H */
