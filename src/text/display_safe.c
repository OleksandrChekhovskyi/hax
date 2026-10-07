/* SPDX-License-Identifier: MIT */
#include "text/display_safe.h"

#include <stddef.h>

#include "buf.h"
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
