/* SPDX-License-Identifier: MIT */
#include "text/display_safe.h"

#include <stddef.h>
#include <string.h>

#include "buf.h"
#include "xalloc.h"
#include "text/utf8.h"

char *sanitize_for_display(const char *text, size_t len)
{
    struct buf sanitized;

    buf_init(&sanitized);
    for (size_t offset = 0; offset < len;) {
        size_t bytes;
        int width = utf8_codepoint_cells(text, len, offset, &bytes);
        if (width < 0)
            buf_append(&sanitized, "?", 1);
        else
            buf_append(&sanitized, text + offset, bytes ? bytes : 1);
        offset += bytes ? bytes : 1;
    }
    return buf_steal(&sanitized);
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
