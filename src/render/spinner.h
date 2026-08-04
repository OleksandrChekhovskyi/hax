/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_SPINNER_H
#define HAX_RENDER_SPINNER_H

/* Thread-safe live indicators written directly to stdout, for interactive terminals only.
 * Operations taking a spinner pointer are NULL-safe no-ops.
 *
 * Between show and hide, callers must not write to stdout; the spinner owns the cursor. */
struct spinner;

#define SPINNER_TOOL_VIEW_ROWS_MAX 8

/* Create a spinner with a copied initial label. NULL or an empty label selects "working...".
 * Returns NULL when stdout is not a terminal or the animation thread cannot start. */
struct spinner *spinner_new(const char *label);
void spinner_free(struct spinner *spinner);

/* Show the labeled spinner at column zero of the current empty row. Hide erases that row. A
 * first appearance from hidden is deferred briefly; any show or hide within the grace period
 * cancels it. */
void spinner_show(struct spinner *spinner);

/* Show the labeled spinner one blank row below the current cursor, then restore the cursor on
 * hide. cursor_col is the current zero-based column. The current line must not wrap. First
 * appearances are deferred as for spinner_show(). */
void spinner_park(struct spinner *spinner, int cursor_col);

/* One prepared tool-status row: fully styled bytes for a single physical row without a line
 * ending, and its painted cell width for reflow-aware repaints. Rows must be clipped to the
 * terminal by the caller; a wrapped row breaks the view's row accounting. */
struct spinner_row {
    const char *bytes;
    int cells;
};

/* Replace the tool-status view with up to SPINNER_TOOL_VIEW_ROWS_MAX copied prepared rows, newest
 * last; excess oldest rows are dropped. Rows are painted verbatim, with the animated glyph
 * overprinting the last row's first cell. Shows the view if it is hidden; content changes repaint
 * on the next animation frame, not synchronously. */
void spinner_set_tool_status_view(struct spinner *spinner, const struct spinner_row *rows,
                                  int count);

/* Erase the visible row and restore the cursor position saved by spinner_park(). */
void spinner_hide(struct spinner *spinner);

/* Hide as spinner_hide(), but begin DEC 2026 synchronized output atomically with the erase and
 * leave the bracket open for the caller's replacement writes. Pair with spinner_swap_end();
 * brackets do not nest, so until then the caller may only write replacement output — no other
 * spinner operation, no other bracket. */
void spinner_swap_begin(struct spinner *spinner);

/* End the swap's synchronized output. Emits nothing when no swap is open. */
void spinner_swap_end(struct spinner *spinner);

/* Immediately replace the labeled spinner's state and discard any deferred request. key identifies
 * a logical state independently of its display text. NULL or empty values select the "working" and
 * "working..." defaults. Inputs are copied. */
void spinner_set_label(struct spinner *spinner, const char *key, const char *label);

/* Request a label change with hysteresis. A new key is displayed only after remaining stable; churn
 * instead returns the display to "working...". Repeating the displayed key updates its text
 * immediately, while repeating a pending key preserves its settling time. Inputs are copied. */
void spinner_request_label(struct spinner *spinner, const char *key, const char *label);

/* Set the labeled spinner's elapsed-time origin to a positive monotonic_ms() value, or disable the
 * counter with zero. The counter appears only for long waits and never on tool-status rows. */
void spinner_set_timer(struct spinner *spinner, long started_at_ms);

/* Return a borrowed, NUL-terminated one-cell UTF-8 glyph selected from monotonic time. */
const char *spinner_glyph_now(void);

/* Cell widths of the physical rows painted by one tool-view frame, for reflow-aware repaints. */
struct spinner_tool_frame {
    int row_widths[SPINNER_TOOL_VIEW_ROWS_MAX];
    int row_count;
};

struct buf;

/* Append one synchronized tool-view repaint frame: climb over the previous frame (NULL on first
 * paint), then overprint every row before erasing stale tails, so terminals without synchronized
 * output never show a blank frame. Rows are painted verbatim with the glyph overprinting the last
 * row's first cell; their widths are recorded in painted. Pure with respect to the terminal. */
void spinner_build_tool_frame(struct buf *frame, const struct spinner_row *rows, int row_count,
                              const char *glyph, int terminal_cols,
                              const struct spinner_tool_frame *previous,
                              struct spinner_tool_frame *painted);

#endif /* HAX_RENDER_SPINNER_H */
