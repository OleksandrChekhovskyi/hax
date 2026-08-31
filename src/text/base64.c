/* SPDX-License-Identifier: MIT */
#include "text/base64.h"

#include <stdint.h>
#include <stdlib.h>

#include "xalloc.h"

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const char B64URL_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static char *encode_alphabet(const void *data, size_t len, const char *alphabet, int padded,
                             size_t *out_len)
{
    size_t out_n = ((len + 2) / 3) * 4;
    char *buf = xmalloc(out_n + 1);
    const unsigned char *in = data;
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        buf[o++] = alphabet[(v >> 18) & 0x3f];
        buf[o++] = alphabet[(v >> 12) & 0x3f];
        buf[o++] = alphabet[(v >> 6) & 0x3f];
        buf[o++] = alphabet[v & 0x3f];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len)
            v |= (uint32_t)in[i + 1] << 8;
        buf[o++] = alphabet[(v >> 18) & 0x3f];
        buf[o++] = alphabet[(v >> 12) & 0x3f];
        if (i + 1 < len)
            buf[o++] = alphabet[(v >> 6) & 0x3f];
        else if (padded)
            buf[o++] = '=';
        if (padded)
            buf[o++] = '=';
    }
    buf[o] = '\0';
    if (out_len)
        *out_len = o;
    return buf;
}

char *base64_encode(const void *data, size_t len, size_t *out_len)
{
    return encode_alphabet(data, len, B64_ALPHABET, 1, out_len);
}

char *base64url_encode(const void *data, size_t len, size_t *out_len)
{
    return encode_alphabet(data, len, B64URL_ALPHABET, 0, out_len);
}

static int base64url_value(unsigned char byte)
{
    if (byte >= 'A' && byte <= 'Z')
        return byte - 'A';
    if (byte >= 'a' && byte <= 'z')
        return byte - 'a' + 26;
    if (byte >= '0' && byte <= '9')
        return byte - '0' + 52;
    if (byte == '-')
        return 62;
    if (byte == '_')
        return 63;
    return -1;
}

unsigned char *base64url_decode(const char *encoded, size_t encoded_len, size_t *out_len)
{
    size_t payload_len = encoded_len;
    while (payload_len > 0 && encoded[payload_len - 1] == '=')
        payload_len--;

    size_t padding = encoded_len - payload_len;
    size_t remainder = payload_len % 4;
    if (padding > 2 || remainder == 1 ||
        (padding > 0 && (encoded_len % 4 != 0 || padding != 4 - remainder)))
        return NULL;

    size_t decoded_len = payload_len / 4 * 3 + (remainder ? remainder - 1 : 0);
    unsigned char *decoded = xmalloc(decoded_len + 1);
    size_t input_offset = 0;
    size_t output_offset = 0;

    while (input_offset + 4 <= payload_len) {
        int values[4];
        for (size_t i = 0; i < 4; i++) {
            values[i] = base64url_value((unsigned char)encoded[input_offset + i]);
            if (values[i] < 0)
                goto malformed;
        }
        decoded[output_offset++] = (unsigned char)((values[0] << 2) | (values[1] >> 4));
        decoded[output_offset++] = (unsigned char)((values[1] << 4) | (values[2] >> 2));
        decoded[output_offset++] = (unsigned char)((values[2] << 6) | values[3]);
        input_offset += 4;
    }

    if (remainder > 0) {
        int first = base64url_value((unsigned char)encoded[input_offset]);
        int second = base64url_value((unsigned char)encoded[input_offset + 1]);
        if (first < 0 || second < 0 || (remainder == 2 && (second & 0x0f) != 0))
            goto malformed;
        decoded[output_offset++] = (unsigned char)((first << 2) | (second >> 4));

        if (remainder == 3) {
            int third = base64url_value((unsigned char)encoded[input_offset + 2]);
            if (third < 0 || (third & 0x03) != 0)
                goto malformed;
            decoded[output_offset++] = (unsigned char)((second << 4) | (third >> 2));
        }
    }

    decoded[output_offset] = '\0';
    if (out_len)
        *out_len = output_offset;
    return decoded;

malformed:
    free(decoded);
    return NULL;
}
