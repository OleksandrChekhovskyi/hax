/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_FOOTER_H
#define HAX_RENDER_FOOTER_H

/* Thread-safe live area below the conversation, written directly to stdout for interactive
 * terminals only: the spinner's status row, below rows of live tool output when there are any,
 * animated until hidden. Operations taking a footer pointer are NULL-safe no-ops.
 *
 * Between show and hide, callers must not write to stdout except through an attached disp, which
 * makes way for the footer; otherwise the footer owns the cursor. */
struct footer;
struct disp;

#define FOOTER_ROWS_MAX 8

/* Create a footer whose status row starts with a copied label. NULL or an empty label selects
 * "working...". Returns NULL when stdout is not a terminal or the animation thread cannot start. */
struct footer *footer_new(const char *label);
void footer_free(struct footer *footer);

/* Let output written through disp pass the footer, which steps aside before each batch of writes
 * and repaints below the output at disp_flush(). disp is borrowed until footer_free(). */
void footer_attach(struct footer *footer, struct disp *disp);

/* Rows from where disp's output ends down to the status row: the status row starts the next
 * block, one blank row below the output's last row. */
int footer_status_gap(const struct disp *disp);

/* Show the status row where the next block of the attached output starts, or at column zero of the
 * current empty row when unattached, in place of any prepared rows. Made while attached output is
 * stepping around the footer, as by footer_set_rows() too, the change paints with that output in
 * one synchronized frame. */
void footer_show(struct footer *footer);

/* Show a hidden, unattached footer's status row after a brief grace period, so a wait of unknown
 * length that ends quickly never blinks. Any show or hide within the grace period cancels it. */
void footer_show_deferred(struct footer *footer);

/* One prepared row: fully styled bytes for a single physical row without a line ending, and its
 * painted cell width. Rows wider than the terminal, as after a resize, are clipped when painted. */
struct footer_row {
    const char *bytes;
    int cells;
};

/* Paint up to FOOTER_ROWS_MAX copied prepared rows, newest last, above a blank row and the status
 * row; excess oldest rows are dropped. Rows are painted verbatim. Shows them if the footer is
 * hidden; content changes repaint on the next animation frame, not synchronously. */
void footer_set_rows(struct footer *footer, const struct footer_row *rows, int count);

/* Erase the visible rows, leaving the cursor where the attached output ends. */
void footer_hide(struct footer *footer);

/* Change the status label as spinner_label_set() and spinner_label_request(), repainting a
 * visible status row when its text changes. Request states of unknown length. Since a set label
 * holds until the next set, set only to reset or for a state known to last seconds while nothing
 * else requests a label. */
void footer_set_label(struct footer *footer, const char *key, const char *label);
void footer_request_label(struct footer *footer, const char *key, const char *label);

/* Returns a label to show in place of the label state, or NULL to show that state. */
typedef const char *(*footer_label_override_fn)(void *user);

/* Consult override, which may be NULL, on every paint, for state that changes outside the
 * footer's control. It runs with the footer locked on any thread, so it must be thread-safe and
 * must not call back into the footer. */
void footer_set_label_override(struct footer *footer, footer_label_override_fn override,
                               void *user);

/* Set the status row's elapsed-time origin to a positive monotonic_ms() value, or disable the
 * counter with zero. The counter appears only for long waits. */
void footer_set_timer(struct footer *footer, long started_at_ms);

struct buf;
struct live_region;

/* Append the moves from the footer's top to where disp's output ends, gap_rows above it, on a
 * screen terminal_cols wide. When an open row ends at the edge, or no longer fits on one screen row
 * at both parked_cols and terminal_cols, output resumes at the start of the row below instead, and
 * the return is nonzero for disp_note_row_break(). Pure with respect to the terminal. */
int footer_build_return(struct buf *out, const struct disp *disp, int gap_rows, int parked_cols,
                        int terminal_cols);

/* How many of row_count prepared rows fit on a screen terminal_rows high, below the output's last
 * row and above the blank and status rows. A taller footer could not climb back to its top, and
 * each repaint would scroll a row into history. */
int footer_rows_that_fit(int row_count, int terminal_rows);

/* Append one synchronized repaint of region with rows, overprinting each row before erasing its
 * stale tail so terminals without synchronized output never show a blank frame. Rows wider than
 * terminal_cols - 1 are clipped to it. The cursor rests at the start of the last row. Pure with
 * respect to the terminal. */
void footer_build_frame(struct buf *frame, struct live_region *region,
                        const struct footer_row *rows, int row_count, int terminal_cols);

#endif /* HAX_RENDER_FOOTER_H */
