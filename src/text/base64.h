/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_BASE64_H
#define HAX_TEXT_BASE64_H

#include <stddef.h>

/* Standard base64 (RFC 4648, `+/` alphabet, `=` padding). Returns a newly allocated,
 * NUL-terminated string. `out_len` may receive its length excluding the terminator. */
char *base64_encode(const void *data, size_t len, size_t *out_len);

/* Unpadded RFC 4648 base64url (`-_` alphabet), the inverse of base64url_decode. Returns a newly
 * allocated, NUL-terminated string. `out_len` may receive its length excluding the terminator. */
char *base64url_encode(const void *data, size_t len, size_t *out_len);

/* Decode padded or unpadded RFC 4648 base64url (`-_` alphabet). Returns a newly allocated byte
 * buffer with an extra NUL terminator, or NULL for malformed or non-canonical input. `out_len` may
 * receive the binary length excluding the terminator. */
unsigned char *base64url_decode(const char *encoded, size_t encoded_len, size_t *out_len);

#endif /* HAX_TEXT_BASE64_H */
