/* SPDX-License-Identifier: MIT */
#include "text/base64.h"

#include <stdint.h>

#include "util.h"

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *base64_encode(const void *data, size_t len, size_t *out_len)
{
    size_t out_n = ((len + 2) / 3) * 4;
    char *buf = xmalloc(out_n + 1);
    const unsigned char *in = data;
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        buf[o++] = B64_ALPHABET[(v >> 18) & 0x3f];
        buf[o++] = B64_ALPHABET[(v >> 12) & 0x3f];
        buf[o++] = B64_ALPHABET[(v >> 6) & 0x3f];
        buf[o++] = B64_ALPHABET[v & 0x3f];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len)
            v |= (uint32_t)in[i + 1] << 8;
        buf[o++] = B64_ALPHABET[(v >> 18) & 0x3f];
        buf[o++] = B64_ALPHABET[(v >> 12) & 0x3f];
        buf[o++] = (i + 1 < len) ? B64_ALPHABET[(v >> 6) & 0x3f] : '=';
        buf[o++] = '=';
    }
    buf[o] = '\0';
    if (out_len)
        *out_len = o;
    return buf;
}
