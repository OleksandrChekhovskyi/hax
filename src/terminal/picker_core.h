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
struct picker_state {
    const struct picker_opts *opts;
    size_t *filtered; /* item indices currently matching the filter */
    size_t n_filtered;
    size_t sel; /* offset into filtered[] of the highlighted row */
    size_t top; /* offset into filtered[] of the first visible row */
    int viewport;
    struct buf query;

    int painted;   /* a paint has happened (so we can reposition over it) */
    int prev_rows; /* rows the last paint emitted (cursor sits on the last) */

    struct termios saved;
    int raw_active;
};

/* Pure filter predicate: case-insensitive, `query` split on spaces into
 * terms, every term must occur as a substring of `text`. An empty or
 * NULL query matches everything. NULL `text` matches only an empty query. */
int picker_core_match(const char *text, const char *query);

/* Rebuild filtered[] from opts against the current query, resetting the
 * selection and window to the first match. */
void picker_core_recompute(struct picker_state *s);

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
