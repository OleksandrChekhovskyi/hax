/* SPDX-License-Identifier: MIT */
#include "text/width.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "buf.h"
#include "xalloc.h"
#include "text/utf8.h"

static size_t codepoint_cells_at(const char *str, size_t length, size_t offset,
                                 size_t *codepoint_bytes)
{
    int cells = utf8_codepoint_cells(str, length, offset, codepoint_bytes);
    return cells < 0 ? 1 : (size_t)cells;
}

/* Combining marks stay with the preceding visible codepoint across cuts. */
static size_t skip_zero_width(const char *str, size_t length, size_t offset)
{
    while (offset < length) {
        size_t codepoint_bytes;
        size_t cells = codepoint_cells_at(str, length, offset, &codepoint_bytes);
        if (cells != 0)
            break;
        offset += codepoint_bytes;
    }
    return offset;
}

static size_t advance_cells(const char *str, size_t length, size_t max_cells)
{
    size_t offset = 0;
    size_t cells = 0;
    while (offset < length && cells < max_cells) {
        size_t codepoint_bytes;
        size_t next_cells = codepoint_cells_at(str, length, offset, &codepoint_bytes);
        if (cells + next_cells > max_cells)
            break;
        cells += next_cells;
        offset += codepoint_bytes;
    }
    return skip_zero_width(str, length, offset);
}

size_t display_cells(const char *str)
{
    if (!str)
        return 0;

    size_t length = strlen(str);
    size_t offset = 0;
    size_t cells = 0;
    while (offset < length) {
        size_t codepoint_bytes;
        cells += codepoint_cells_at(str, length, offset, &codepoint_bytes);
        offset += codepoint_bytes;
    }
    return cells;
}

char *truncate_for_display(const char *str, size_t max_cells)
{
    if (!str)
        return xstrdup("");

    size_t length = strlen(str);
    if (length <= max_cells || advance_cells(str, length, max_cells) == length)
        return xstrdup(str);

    size_t content_cells = max_cells < 4 ? max_cells : max_cells - 3;
    size_t content_bytes = advance_cells(str, length, content_cells);
    size_t ellipsis_bytes = max_cells < 4 ? 0 : 3;
    char *result = xmalloc(content_bytes + ellipsis_bytes + 1);
    memcpy(result, str, content_bytes);
    memcpy(result + content_bytes, "...", ellipsis_bytes);
    result[content_bytes + ellipsis_bytes] = '\0';
    return result;
}

/* May return zero rather than exceed max_cells; see forced_row_end. */
static size_t strict_break_pos(const char *str, size_t length, size_t max_cells,
                               size_t *next_offset)
{
    size_t offset = 0;
    size_t cells = 0;
    size_t last_space = SIZE_MAX;
    while (offset < length) {
        size_t codepoint_bytes;
        size_t next_cells = codepoint_cells_at(str, length, offset, &codepoint_bytes);
        if (cells + next_cells > max_cells) {
            /* A boundary space separates rows and does not consume a content cell. */
            if (str[offset] == ' ' && cells == max_cells)
                last_space = offset;
            break;
        }
        if (str[offset] == ' ')
            last_space = offset;
        cells += next_cells;
        offset += codepoint_bytes;
    }

    if (offset >= length) {
        *next_offset = length;
        return length;
    }
    if (last_space == SIZE_MAX) {
        size_t row_end = advance_cells(str, length, max_cells);
        *next_offset = row_end;
        return row_end;
    }

    size_t row_end = last_space;
    while (row_end > 0 && str[row_end - 1] == ' ')
        row_end--;
    *next_offset = last_space + 1;
    return row_end;
}

/* Taking one oversized codepoint preserves forward progress where a row cannot be empty. */
static size_t forced_row_end(const char *str, size_t length)
{
    return skip_zero_width(str, length, utf8_next(str, length, 0));
}

size_t wrap_break_pos(const char *str, size_t length, size_t max_cells, size_t *next_offset)
{
    assert(max_cells >= 1);
    size_t next = 0;
    size_t row_end = strict_break_pos(str, length, max_cells, &next);
    if (row_end == 0 && next == 0 && length > 0) {
        row_end = forced_row_end(str, length);
        next = row_end;
    }
    if (next_offset)
        *next_offset = next;
    return row_end;
}

size_t wrap_row_bytes(const char *str, size_t max_cells, size_t *separator_bytes)
{
    size_t length = strlen(str);
    size_t paragraph_len = strcspn(str, "\n");
    size_t next_offset;
    size_t row_bytes = wrap_break_pos(str, paragraph_len, max_cells, &next_offset);
    /* A row ending at the paragraph consumes the newline that ended it. */
    if (next_offset == paragraph_len && paragraph_len < length)
        next_offset++;
    *separator_bytes = next_offset - row_bytes;
    return row_bytes;
}

/* Reflowed rows give up an intact token only when keeping it would leave more than this many cells,
 * or half a narrow row, unused: a long pattern or path is more legible split than pushed out of
 * view. */
#define REFLOW_MAX_SLACK_CELLS 16

/* Each mark ends a unit of a command-line token: a regex alternative, path component, list item,
 * command, or option name. */
static int is_reflow_break_mark(char c)
{
    return c != '\0' && strchr("|/,;&=", c) != NULL;
}

/* Choose one reflowed row's end: a space, else a mark, each only within the slack limit, else the
 * last codepoint boundary that fits. May return zero rather than exceed max_cells. */
static size_t reflow_break_pos(const char *str, size_t length, size_t max_cells,
                               size_t *next_offset)
{
    size_t offset = 0;
    size_t cells = 0;
    size_t space = SIZE_MAX;
    size_t space_cells = 0;
    size_t mark_break = SIZE_MAX;
    size_t mark_break_cells = 0;
    int after_mark = 0;
    while (offset < length) {
        size_t codepoint_bytes;
        size_t next_cells = codepoint_cells_at(str, length, offset, &codepoint_bytes);
        /* A mark break lands before the next visible non-mark codepoint, keeping runs such as "&&"
         * whole and combining marks with their mark. */
        if (str[offset] == ' ') {
            space = offset;
            space_cells = cells;
        } else if (after_mark && next_cells > 0 && !is_reflow_break_mark(str[offset])) {
            mark_break = offset;
            mark_break_cells = cells;
        }
        if (next_cells > 0)
            after_mark = is_reflow_break_mark(str[offset]);
        if (cells + next_cells > max_cells)
            break;
        cells += next_cells;
        offset += codepoint_bytes;
    }
    if (offset >= length) {
        *next_offset = length;
        return length;
    }

    size_t max_slack = max_cells / 2;
    if (max_slack > REFLOW_MAX_SLACK_CELLS)
        max_slack = REFLOW_MAX_SLACK_CELLS;
    if (space != SIZE_MAX && max_cells - space_cells <= max_slack) {
        size_t row_end = space;
        while (row_end > 0 && str[row_end - 1] == ' ')
            row_end--;
        *next_offset = space + 1;
        return row_end;
    }
    if (mark_break != SIZE_MAX && max_cells - mark_break_cells <= max_slack) {
        *next_offset = mark_break;
        return mark_break;
    }
    *next_offset = offset;
    return offset;
}

char *reflow_for_display(const char *str, int first_row_cells, int other_row_cells, int max_rows,
                         int last_row_reserve)
{
    if (!str)
        return xstrdup("");
    if (max_rows < 1)
        max_rows = 1;
    if (first_row_cells < 1)
        first_row_cells = 1;
    if (other_row_cells < 1)
        other_row_cells = 1;
    if (last_row_reserve < 0)
        last_row_reserve = 0;

    size_t length = strlen(str);
    int single_row_cells = first_row_cells - last_row_reserve;
    if (single_row_cells < 1)
        single_row_cells = 1;
    if (length <= (size_t)single_row_cells)
        return xstrdup(str);

    struct buf result;
    buf_init(&result);
    size_t offset = 0;
    for (int row = 0; row < max_rows; row++) {
        int row_cells = row == 0 ? first_row_cells : other_row_cells;
        /* Any emitted row may become the last, so all rows leave room for the suffix. */
        int content_cells = row_cells - last_row_reserve;
        if (content_cells < 1)
            content_cells = 1;

        size_t remaining = length - offset;
        if (advance_cells(str + offset, remaining, (size_t)content_cells) == remaining) {
            buf_append(&result, str + offset, remaining);
            break;
        }

        if (row == max_rows - 1) {
            int before_ellipsis_cells = content_cells - 3;
            if (before_ellipsis_cells < 1) {
                size_t row_bytes = advance_cells(str + offset, remaining, (size_t)content_cells);
                buf_append(&result, str + offset, row_bytes);
                break;
            }
            size_t next_offset;
            size_t row_bytes = reflow_break_pos(str + offset, remaining,
                                                (size_t)before_ellipsis_cells, &next_offset);
            buf_append(&result, str + offset, row_bytes);
            buf_append(&result, "...", 3);
            break;
        }

        size_t next_offset;
        size_t row_bytes =
            reflow_break_pos(str + offset, remaining, (size_t)content_cells, &next_offset);
        if (next_offset == 0) {
            row_bytes = forced_row_end(str + offset, remaining);
            next_offset = row_bytes;
        }
        buf_append(&result, str + offset, row_bytes);
        buf_append(&result, "\n", 1);
        offset += next_offset;
    }
    return buf_steal(&result);
}
