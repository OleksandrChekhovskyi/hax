/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_WIDTH_H
#define HAX_TERMINAL_WIDTH_H

/* Physical terminal width on stdout, or 120 when unavailable. */
int term_width(void);

#define DISPLAY_WIDTH_CAP       100
#define DISPLAY_WIDTH_TOLERANCE 10

/* Automatic content width for terminal_width. Widths within DISPLAY_WIDTH_TOLERANCE above the cap
 * are used in full; wider values are capped. The result has a 20-column floor. */
int auto_display_width(int terminal_width);

/* Width used for content layout. "auto" uses auto_display_width(), "terminal" removes the upper
 * bound, and an integer >= 20 selects an exact width. */
int display_width(void);

/* Physical rows that previously painted rows of the given cell widths occupy after an xterm-style
 * reflow to terminal_cols. Empty rows still occupy one physical row. terminal_cols must be
 * positive. */
int reflow_physical_rows(const int *row_widths, int count, int terminal_cols);

#endif /* HAX_TERMINAL_WIDTH_H */
