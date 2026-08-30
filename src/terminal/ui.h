/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_UI_H
#define HAX_TERMINAL_UI_H

/* User-facing status lines and listing rows, printed to stdout so they interleave with the
 * on-screen conversation. Not for startup/CLI errors, which print "hax: ..." to stderr before the
 * REPL exists (hax_err() in diag.h), nor for mid-stream provider errors, which flow through the
 * unified EV_ERROR event and the agent's display layer. */

/* One status line: ui_error (red) reports a failure, ui_note (dim) an informational aside. Color
 * is gated on stdout being a TTY; the trailing newline is appended. Callers pass a printf-style
 * message with no color codes and no newline. */
__attribute__((format(printf, 1, 2))) void ui_error(const char *fmt, ...);
__attribute__((format(printf, 1, 2))) void ui_note(const char *fmt, ...);

/* Word-wrapped listing rows. Style arguments are resolved open sequences (theme_open(), ANSI_DIM,
 * or "" for unstyled); an empty open suppresses the closing reset too, so colorless output
 * carries no escapes. */

/* Below this text-column budget the label column is dropped and text stacks under the label. */
#define UI_ROW_MIN_TEXT_CELLS 20
#define UI_ROW_STACKED_INDENT 4

/* Print text word-wrapped at `columns` with continuation rows padded to `indent`; the cursor
 * already sits at `indent`. Embedded newlines force row breaks. Styling wraps each row so runs
 * never span physical lines. */
void ui_wrapped_rows(const char *text, int indent, int columns, const char *style_open);

/* Print one listing row: a two-space margin, then `label` (ASCII, so byte length equals cell
 * width), then `text` wrapped from `text_column`. When fewer than UI_ROW_MIN_TEXT_CELLS remain
 * beside the label column, `text` stacks under the label at UI_ROW_STACKED_INDENT instead. */
void ui_label_row(const char *label, const char *label_open, const char *text,
                  const char *text_open, int text_column, int columns);

#endif /* HAX_TERMINAL_UI_H */
