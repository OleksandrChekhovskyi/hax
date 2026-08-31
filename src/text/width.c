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

/* Unlike wrapping, ellipsis truncation may return zero rather than exceed max_cells. */
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
        if (next_offset)
            *next_offset = length;
        return length;
    }
    if (last_space == SIZE_MAX) {
        size_t row_end = advance_cells(str, length, max_cells);
        if (next_offset)
            *next_offset = row_end;
        return row_end;
    }

    size_t row_end = last_space;
    while (row_end > 0 && str[row_end - 1] == ' ')
        row_end--;
    if (next_offset)
        *next_offset = last_space + 1;
    return row_end;
}

size_t wrap_break_pos(const char *str, size_t length, size_t max_cells, size_t *next_offset)
{
    assert(max_cells >= 1);
    size_t next = 0;
    size_t row_end = strict_break_pos(str, length, max_cells, &next);
    if (row_end == 0 && next == 0 && length > 0) {
        /* Taking one oversized codepoint preserves forward progress. */
        row_end = skip_zero_width(str, length, utf8_next(str, length, 0));
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
            size_t row_bytes =
                strict_break_pos(str + offset, remaining, (size_t)before_ellipsis_cells, NULL);
            buf_append(&result, str + offset, row_bytes);
            buf_append(&result, "...", 3);
            break;
        }

        size_t next_offset;
        size_t row_bytes =
            wrap_break_pos(str + offset, remaining, (size_t)content_cells, &next_offset);
        buf_append(&result, str + offset, row_bytes);
        buf_append(&result, "\n", 1);
        offset += next_offset;
    }
    return buf_steal(&result);
}

/* Bounds invisible byte growth while preserving ordinary combining sequences. */
#define MAX_ZERO_WIDTH_PER_BASE 8

char *flatten_for_display(const char *str)
{
    if (!str)
        return xstrdup("");

    size_t length = strlen(str);
    /* Every transformation preserves, removes, or replaces input bytes with one byte. */
    char *result = xmalloc(length + 1);
    size_t result_length = 0;
    int previous_was_space = 1;
    int zero_width_run = 0;
    size_t offset = 0;
    while (offset < length) {
        unsigned char byte = (unsigned char)str[offset];
        if (byte < 0x80) {
            int is_space = byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' ||
                           byte < 0x20 || byte == 0x7f;
            if (is_space) {
                if (!previous_was_space) {
                    result[result_length++] = ' ';
                    previous_was_space = 1;
                }
            } else {
                result[result_length++] = (char)byte;
                previous_was_space = 0;
            }
            zero_width_run = 0;
            offset++;
            continue;
        }

        size_t codepoint_bytes;
        int cells = utf8_codepoint_cells(str, length, offset, &codepoint_bytes);
        if (cells < 0) {
            result[result_length++] = '?';
            zero_width_run = 0;
            previous_was_space = 0;
        } else if (cells == 0) {
            if (zero_width_run < MAX_ZERO_WIDTH_PER_BASE) {
                memcpy(result + result_length, str + offset, codepoint_bytes);
                result_length += codepoint_bytes;
                zero_width_run++;
            }
        } else {
            memcpy(result + result_length, str + offset, codepoint_bytes);
            result_length += codepoint_bytes;
            zero_width_run = 0;
            previous_was_space = 0;
        }
        offset += codepoint_bytes;
    }

    if (result_length > 0 && result[result_length - 1] == ' ')
        result_length--;
    result[result_length] = '\0';
    return result;
}
