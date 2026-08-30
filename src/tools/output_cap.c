/* SPDX-License-Identifier: MIT */
#include "tools/output_cap.h"

#include <stdio.h>

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
