/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_BASE64_H
#define HAX_TEXT_BASE64_H

#include <stddef.h>

/* Standard base64 (RFC 4648, `+/` alphabet, `=` padding). Returns a
 * freshly-allocated NUL-terminated string the caller frees; never NULL
 * (allocation aborts on OOM). `out_len` (optional) receives the encoded
 * length excluding the terminator. */
char *base64_encode(const void *data, size_t len, size_t *out_len);

#endif /* HAX_TEXT_BASE64_H */
