/* SPDX-License-Identifier: MIT */
#ifndef HAX_DIFF_COLOR_H
#define HAX_DIFF_COLOR_H

#include <stddef.h>

/* Unified-diff line classification shared by the per-line coloring
 * renderers (the live tool preview and the transcript view). Pure
 * classification — each renderer maps the kinds to its own palette:
 * the live preview dims everything but adds/removes to match its
 * all-dim preview rows, while the transcript keeps context on the
 * default foreground like any other tool output and dims only the
 * metadata lines.
 *
 * Callers keep their own `in_hunk` latch: it starts at 0 and is set
 * once the first "@@" hunk header has been rendered. Diffs here are
 * always single-file (one header block up top), so it never resets. */

enum diff_line_kind {
    DIFF_LINE_ADD,     /* "+" addition */
    DIFF_LINE_REMOVE,  /* "-" removal */
    DIFF_LINE_META,    /* "--- "/"+++ " headers, "@@" markers, "\ No newline" */
    DIFF_LINE_CONTEXT, /* space-prefixed hunk-body line */
};

/* Classify one diff line. Inside a hunk every line carries a one-char
 * prefix, so the prefix alone classifies and the file-header patterns
 * are never re-tested — which keeps a removed "-- x" (rendered
 * "--- x") or an added "++ x" (rendered "+++ x") classified as a
 * change instead of a header. Before the first hunk, unrecognized
 * lines fall back to META (degenerate input only). */
enum diff_line_kind diff_line_classify(const char *line, size_t len, int in_hunk);

/* The unified-diff file-header lines ("--- a/x" / "+++ b/x"). Only the
 * pair *before* the first hunk are headers; an identical-looking line
 * inside a hunk body is real removed/added content (handled by the
 * in_hunk latch at the call site). */
int diff_is_file_header(const char *line, size_t len);

#endif /* HAX_DIFF_COLOR_H */
