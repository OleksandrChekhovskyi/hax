/* SPDX-License-Identifier: MIT */
#include "text/url.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "buf.h"
#include "xalloc.h"
#include "text/fmt.h"

char *url_trim_trailing_slashes(const char *url)
{
    size_t length = strlen(url);
    while (length > 0 && url[length - 1] == '/')
        length--;
    char *result = xmalloc(length + 1);
    memcpy(result, url, length);
    result[length] = '\0';
    return result;
}

void url_encode_append(struct buf *out, const char *value)
{
    for (const char *cursor = value; *cursor; cursor++) {
        unsigned char c = (unsigned char)*cursor;
        if (isalnum(c) || strchr("-._~", c)) {
            buf_append(out, cursor, 1);
        } else {
            char escaped[4];
            snprintf(escaped, sizeof(escaped), "%%%02X", c);
            buf_append_str(out, escaped);
        }
    }
}

char *url_encode(const char *value)
{
    struct buf encoded;
    buf_init(&encoded);
    url_encode_append(&encoded, value);
    char *result = buf_steal(&encoded);
    return result ? result : xstrdup("");
}

char *url_decode(const char *encoded, size_t len)
{
    struct buf decoded;
    buf_init(&decoded);
    for (size_t i = 0; i < len; i++) {
        char c = encoded[i];
        if (c == '+') {
            buf_append(&decoded, " ", 1);
            continue;
        }
        uint32_t value;
        if (c == '%' && i + 2 < len && parse_hex(encoded + i + 1, 2, &value)) {
            char byte = (char)value;
            buf_append(&decoded, &byte, 1);
            i += 2;
            continue;
        }
        buf_append(&decoded, &c, 1);
    }
    char *result = buf_steal(&decoded);
    return result ? result : xstrdup("");
}
