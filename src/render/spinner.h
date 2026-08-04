/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_SPINNER_H
#define HAX_RENDER_SPINNER_H

/* Thread-safe, single-row indicators written directly to stdout. Operations taking a spinner
 * pointer are NULL-safe. The labeled spinner animates only when stdout is a TTY; the initial
 * tool-status row is painted synchronously even on non-TTY stdout.
 *
 * Between show and hide, callers must not write to stdout; the spinner owns the cursor. */
struct spinner;

/* Create a spinner with a copied initial label. NULL or an empty label selects "working...". */
struct spinner *spinner_new(const char *label);
void spinner_free(struct spinner *spinner);

/* Show the labeled spinner at column zero of the current empty row. Hide erases that row. */
void spinner_show(struct spinner *spinner);

/* Show the labeled spinner one blank row below the current cursor, then restore the cursor on hide.
 * cursor_col is the current zero-based column. The current line must not wrap. */
void spinner_park(struct spinner *spinner, int cursor_col);

/* Show a live tool-output row containing an animated gutter and copied single-line content. Any
 * parked spinner is restored first, so the status occupies the caller's current row. */
void spinner_show_tool_status(struct spinner *spinner, const char *content);

/* Copy new content for a visible tool-status row. A running animation thread repaints it on the
 * next frame rather than synchronously, limiting terminal writes to the frame rate. */
void spinner_set_tool_status_content(struct spinner *spinner, const char *content);

/* Erase the visible row and restore the cursor position saved by spinner_park(). */
void spinner_hide(struct spinner *spinner);

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

#endif /* HAX_RENDER_SPINNER_H */
