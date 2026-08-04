/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_WIDTH_H
#define HAX_TERMINAL_WIDTH_H

/* Physical terminal width on stdout, or 120 when unavailable. */
int term_width(void);

#define DISPLAY_WIDTH_CAP 100

/* Width used for content layout. "auto" clamps the terminal width to [20, DISPLAY_WIDTH_CAP],
 * "terminal" removes the upper bound, and an integer >= 20 selects an exact width. */
int display_width(void);

/* Physical rows that previously painted rows of the given cell widths occupy after an xterm-style
 * reflow to terminal_cols. Empty rows still occupy one physical row. terminal_cols must be
 * positive. */
int reflow_physical_rows(const int *row_widths, int count, int terminal_cols);

#endif /* HAX_TERMINAL_WIDTH_H */
