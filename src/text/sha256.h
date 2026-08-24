/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_SHA256_H
#define HAX_TEXT_SHA256_H

#include <stddef.h>

#define SHA256_DIGEST_LEN 32

/* One-shot SHA-256 of `len` bytes at `data`. */
void sha256(const void *data, size_t len, unsigned char digest[SHA256_DIGEST_LEN]);

#endif /* HAX_TEXT_SHA256_H */
