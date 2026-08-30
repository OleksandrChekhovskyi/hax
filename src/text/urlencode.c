/* SPDX-License-Identifier: MIT */
#include "text/urlencode.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "buf.h"
#include "util.h"

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

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
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
        if (c == '%' && i + 2 < len) {
            int hi = hex_value(encoded[i + 1]);
            int lo = hex_value(encoded[i + 2]);
            if (hi >= 0 && lo >= 0) {
                char byte = (char)(hi << 4 | lo);
                buf_append(&decoded, &byte, 1);
                i += 2;
                continue;
            }
        }
        buf_append(&decoded, &c, 1);
    }
    char *result = buf_steal(&decoded);
    return result ? result : xstrdup("");
}
