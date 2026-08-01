/* SPDX-License-Identifier: MIT */
#ifndef HAX_DIFF_COLOR_H
#define HAX_DIFF_COLOR_H

#include <stddef.h>

/* Renderers map unified-diff line kinds to their own styles. Callers track whether the first hunk
 * has started; input is single-file, so that state never resets. */

enum diff_line_kind {
    DIFF_LINE_ADD,
    DIFF_LINE_REMOVE,
    DIFF_LINE_META,    /* file headers, hunk markers, and no-newline markers */
    DIFF_LINE_CONTEXT, /* space-prefixed hunk-body line */
};

/* Inside a hunk, classify only by prefix so changed content resembling a file header remains an
 * addition or removal. Before the first hunk, unrecognized lines are metadata. */
enum diff_line_kind diff_line_classify(const char *line, size_t length, int in_hunk);

/* Return whether `line` is a unified-diff file header. Call only before the first hunk. */
int diff_is_file_header(const char *line, size_t length);

#endif /* HAX_DIFF_COLOR_H */
