/* SPDX-License-Identifier: MIT */
#include "text/shell_quote.h"

#include "buf.h"

char *shell_single_quote(const char *str)
{
    struct buf quoted;
    buf_init(&quoted);
    buf_append(&quoted, "'", 1);
    for (; str && *str; str++) {
        if (*str == '\'')
            buf_append_str(&quoted, "'\\''");
        else
            buf_append(&quoted, str, 1);
    }
    buf_append(&quoted, "'", 1);
    return buf_steal(&quoted);
}
