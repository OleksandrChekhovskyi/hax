/* SPDX-License-Identifier: MIT */
#include "tools/output_cap.h"

#include <stdio.h>
#include <string.h>

#include "buf.h"
#include "config.h"

size_t output_cap_bytes(void)
{
    return (size_t)config_size("tool_output_cap");
}

char *cap_line_lengths(const char *data, size_t length, size_t max_line_bytes, size_t *out_len)
{
    struct buf result;
    buf_init(&result);
    size_t offset = 0;
    while (offset < length) {
        size_t line_start = offset;
        while (offset < length && data[offset] != '\n')
            offset++;
        size_t line_length = offset - line_start;
        if (line_length > max_line_bytes) {
            buf_append(&result, data + line_start, max_line_bytes);
            char marker[64];
            int marker_length = snprintf(marker, sizeof(marker), "...[%zu bytes elided]",
                                         line_length - max_line_bytes);
            buf_append(&result, marker, (size_t)marker_length);
        } else {
            buf_append(&result, data + line_start, line_length);
        }
        if (offset < length) {
            buf_append(&result, "\n", 1);
            offset++;
        }
    }
    if (out_len)
        *out_len = result.len;
    return buf_steal(&result);
}

size_t whole_lines_head(const char *data, size_t length, size_t max_lines, size_t *lines_out)
{
    size_t end = 0;
    size_t lines = 0;
    for (size_t i = 0; i < length && lines < max_lines; i++) {
        if (data[i] == '\n') {
            end = i + 1;
            lines++;
        }
    }
    if (lines_out)
        *lines_out = lines;
    return end;
}

size_t whole_lines_tail(const char *data, size_t length, int mid_line, size_t max_lines,
                        size_t *lines_out)
{
    size_t start = 0;
    if (mid_line && length > 0) {
        const char *newline = memchr(data, '\n', length);
        if (newline && (size_t)(newline - data) + 1 < length)
            start = (size_t)(newline - data) + 1;
    }

    size_t lines = length > start && data[length - 1] != '\n';
    for (size_t i = start; i < length; i++)
        lines += data[i] == '\n';
    for (; lines > max_lines; lines--) {
        const char *newline = memchr(data + start, '\n', length - start);
        start = newline ? (size_t)(newline - data) + 1 : length;
    }
    if (lines_out)
        *lines_out = lines;
    return start;
}
