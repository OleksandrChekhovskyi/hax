/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_DISPLAY_SAFE_H
#define HAX_TEXT_DISPLAY_SAFE_H

#include <stddef.h>

/* Preparation of untrusted UTF-8 for writing to a terminal. These helpers require
 * locale_init_utf8() to keep valid non-ASCII text. */

/* Return an allocated copy of the first `len` bytes of `text` with each malformed, control, or
 * format codepoint that could hide or rearrange terminal content replaced by '?'. */
char *sanitize_for_display(const char *text, size_t len);

/* Prepare untrusted UTF-8 for one-line display: collapse ASCII whitespace, replace malformed or
 * direction-changing codepoints, and bound combining-mark runs. Returns an allocated string; NULL
 * input becomes empty. */
char *flatten_for_display(const char *str);

#endif /* HAX_TEXT_DISPLAY_SAFE_H */
