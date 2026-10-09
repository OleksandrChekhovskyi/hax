/* SPDX-License-Identifier: MIT */
#ifndef HAX_TESTS_OUTPUT_H
#define HAX_TESTS_OUTPUT_H

#include <stddef.h>

/* Fixtures for tests that assert on printed output. Returned strings are allocated and owned by
 * the caller. */

/* Run `body` with stdout redirected to a scratch file, then restore it; return what it printed. */
char *t_capture_stdout(void (*body)(void *), void *user);

/* Return `text` without its CSI sequences, so assertions see plain text whatever SGR styling the
 * theme resolved to. */
char *t_strip_sgr(const char *text);

/* Fail for each row of plain `text` wider than `max_cells` display cells. */
void t_expect_rows_fit(const char *text, size_t max_cells);

#endif /* HAX_TESTS_OUTPUT_H */
