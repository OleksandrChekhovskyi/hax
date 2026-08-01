/* SPDX-License-Identifier: MIT */
#include "render/diff_color.h"

#include <string.h>

/* Hunk content always has a change/context prefix, so bare markers are metadata in any state. */
enum diff_line_kind diff_line_classify(const char *line, size_t length, int in_hunk)
{
    if (length >= 1 && line[0] == '\\')
        return DIFF_LINE_META;
    if (length >= 2 && memcmp(line, "@@", 2) == 0)
        return DIFF_LINE_META;
    if (in_hunk) {
        if (length >= 1 && line[0] == '+')
            return DIFF_LINE_ADD;
        if (length >= 1 && line[0] == '-')
            return DIFF_LINE_REMOVE;
        return DIFF_LINE_CONTEXT;
    }

    /* Preserve add/remove styling for malformed diffs without a hunk header. */
    if (diff_is_file_header(line, length))
        return DIFF_LINE_META;
    if (length >= 1 && line[0] == '+')
        return DIFF_LINE_ADD;
    if (length >= 1 && line[0] == '-')
        return DIFF_LINE_REMOVE;
    return DIFF_LINE_META;
}

int diff_is_file_header(const char *line, size_t length)
{
    return length >= 4 && (memcmp(line, "--- ", 4) == 0 || memcmp(line, "+++ ", 4) == 0);
}
