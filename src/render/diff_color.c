/* SPDX-License-Identifier: MIT */
#include "render/diff_color.h"

#include <string.h>

/* The "@@" and "\ No newline" markers match up front in either position
 * — a hunk-body content line never starts with a bare "@@"/"\" (it
 * always carries a +/-/space prefix), so that's unambiguous and keeps
 * inter-hunk separators classified as META in a multi-hunk diff. */
enum diff_line_kind diff_line_classify(const char *line, size_t len, int in_hunk)
{
    if (len >= 1 && line[0] == '\\')
        return DIFF_LINE_META;
    if (len >= 2 && memcmp(line, "@@", 2) == 0)
        return DIFF_LINE_META;
    if (in_hunk) {
        if (len >= 1 && line[0] == '+')
            return DIFF_LINE_ADD;
        if (len >= 1 && line[0] == '-')
            return DIFF_LINE_REMOVE;
        return DIFF_LINE_CONTEXT;
    }
    /* Before the first hunk: file headers, then fall back to prefix
     * classification so stray +/- lines in degenerate input with no
     * "@@" header still read as additions/removals. */
    if (diff_is_file_header(line, len))
        return DIFF_LINE_META;
    if (len >= 1 && line[0] == '+')
        return DIFF_LINE_ADD;
    if (len >= 1 && line[0] == '-')
        return DIFF_LINE_REMOVE;
    return DIFF_LINE_META;
}

int diff_is_file_header(const char *line, size_t len)
{
    return len >= 4 && (memcmp(line, "--- ", 4) == 0 || memcmp(line, "+++ ", 4) == 0);
}
