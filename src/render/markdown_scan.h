/* SPDX-License-Identifier: MIT */
#ifndef HAX_MARKDOWN_SCAN_H
#define HAX_MARKDOWN_SCAN_H

#include <stddef.h>

/* Pure classification of a physical line prefix. An incomplete result may still carry a definite
 * block kind: for example, `* ` is already a bullet but could become a thematic break. */
enum md_line_kind {
    MD_LINE_INCOMPLETE,
    MD_LINE_TEXT,
    MD_LINE_BLANK,
    MD_LINE_HEADING,
    MD_LINE_FENCE,
    MD_LINE_THEMATIC,
    MD_LINE_BULLET,
    MD_LINE_ORDERED,
    MD_LINE_BLOCKQUOTE,
    MD_LINE_PIPE,
};

struct md_line_info {
    enum md_line_kind kind;
    size_t indent_length;
    size_t marker_length;
    char marker;
    /* False when more bytes may refine the result; kind can still be a definite block prefix. */
    int classification_complete;
    int normalize_indent;
};

/* final resolves ambiguous prefixes against EOF instead of requesting more input. */
struct md_line_info md_scan_line(const char *line, size_t length, int final);

#endif /* HAX_MARKDOWN_SCAN_H */
