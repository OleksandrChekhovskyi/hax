/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_URLENCODE_H
#define HAX_TEXT_URLENCODE_H

#include <stddef.h>

#include "buf.h"

/* URL encoding for query strings and form-urlencoded bodies: RFC 3986 unreserved characters pass
 * through and every other byte becomes %XX, so encoded values are safe in both positions. */

/* Append the URL-encoded form of `value` to `out`. */
void url_encode_append(struct buf *out, const char *value);

/* The URL-encoded form of `value` as an owned string. */
char *url_encode(const char *value);

/* Decode `len` bytes of a query or form value: `%XX` and `+` (space) are decoded, and truncated
 * or invalid escapes pass through literally rather than failing, since redirect queries arrive
 * from outside hax's control. Returns an owned string. */
char *url_decode(const char *encoded, size_t len);

#endif /* HAX_TEXT_URLENCODE_H */
