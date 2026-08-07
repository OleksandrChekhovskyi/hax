/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_WIDTH_H
#define HAX_TEXT_WIDTH_H

#include <stddef.h>

/* Cell-accurate measurement, truncation, and wrapping of UTF-8 text. These helpers require
 * locale_init_utf8() for accurate non-ASCII widths. */

/* Return the visual cell width of a NUL-terminated UTF-8 string. NULL has width zero. */
size_t display_cells(const char *str);

/* Return a newly allocated string fitting within max_cells. Truncated strings end in "..." when at
 * least four cells are available. NULL is treated as an empty string. */
char *truncate_for_display(const char *str, size_t max_cells);

/* Choose a UTF-8 byte boundary for one row of at most max_cells. Word breaks consume one separating
 * ASCII space: the return value ends the current row and next_offset starts the next. If no word
 * break fits, the function uses a codepoint boundary. max_cells must be positive. */
size_t wrap_break_pos(const char *str, size_t length, size_t max_cells, size_t *next_offset);

/* Return the byte length of the next display row of a NUL-terminated string, breaking at word
 * boundaries or an embedded newline. separator_bytes receives the consumed break bytes between
 * this row and the next; advance by the sum of both to continue. max_cells must be positive. */
size_t wrap_row_bytes(const char *str, size_t max_cells, size_t *separator_bytes);

/* Reflow a string to at most max_rows. first_row_cells and other_row_cells are row budgets;
 * last_row_reserve leaves room for a caller-owned suffix. Rows are joined by '\n', and overflow is
 * marked with "..." when space permits. Returns an allocated string; NULL input becomes empty. */
char *reflow_for_display(const char *str, int first_row_cells, int other_row_cells, int max_rows,
                         int last_row_reserve);

/* Prepare untrusted UTF-8 for one-line display: collapse ASCII whitespace, replace malformed or
 * direction-changing codepoints, and bound combining-mark runs. Returns an allocated string; NULL
 * input becomes empty. */
char *flatten_for_display(const char *str);

#endif /* HAX_TEXT_WIDTH_H */
