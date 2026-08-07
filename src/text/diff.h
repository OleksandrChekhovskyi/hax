/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_DIFF_H
#define HAX_TEXT_DIFF_H

#include <stddef.h>

/* Generate a unified diff (three lines of context) between two in-memory buffers with the
 * in-tree line diff, so no external diff(1) is needed.
 *
 * a_label / b_label are the strings emitted on the `--- ` / `+++ ` header lines (e.g. "a/path",
 * "b/path", or "/dev/null" for absent files).
 *
 * Returns a freshly allocated NUL-terminated diff string; empty string ("") means the inputs are
 * byte-identical. The output is sanitized to valid UTF-8 (NUL bytes and malformed sequences
 * become U+FFFD) so it can travel through JSON-bound tool results. Never returns NULL.
 *
 * Cost is proportional to the changed region, not the file: per-line work only covers the byte
 * range that differs, and the search runs in linear space under a global work budget, past which
 * affected regions coarsen into replacement blocks rather than growing the cost. */
char *make_unified_diff(const char *a, size_t a_len, const char *b, size_t b_len,
                        const char *a_label, const char *b_label);

#endif /* HAX_TEXT_DIFF_H */
