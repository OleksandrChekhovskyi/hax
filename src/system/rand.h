/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_RAND_H
#define HAX_SYSTEM_RAND_H

#include <stddef.h>

/* Fill `out` with `len` bytes from the system entropy source. Aborts on entropy failure. */
void random_bytes(void *out, size_t len);

/* Write a lowercase UUIDv4 (36 bytes plus the NUL terminator). Aborts on entropy failure. */
void gen_uuid_v4(char out[37]);

#endif /* HAX_SYSTEM_RAND_H */
