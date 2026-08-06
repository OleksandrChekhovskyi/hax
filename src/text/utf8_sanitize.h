/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_UTF8_SANITIZE_H
#define HAX_TEXT_UTF8_SANITIZE_H

#include <stddef.h>

/* Each invalid input byte becomes the three-byte U+FFFD encoding. A feed may also release up to
 * three bytes carried from the previous chunk. */
#define UTF8_SANITIZE_FEED_MAX(input_len) ((input_len) * 3 + 9)
#define UTF8_SANITIZE_FLUSH_MAX           9

/* Incremental RFC 3629 sanitizer. NUL is also replaced because downstream JSON and terminal paths
 * use NUL-terminated strings. Call flush at end of input to replace any incomplete sequence. */
struct utf8_sanitizer {
    unsigned char pending[4];
    unsigned char pending_len;
};

void utf8_sanitizer_init(struct utf8_sanitizer *sanitizer);

/* Sanitize a counted chunk and return the number of output bytes. Output must have
 * UTF8_SANITIZE_FEED_MAX(input_len) bytes available and must not overlap input. */
size_t utf8_sanitizer_feed(struct utf8_sanitizer *sanitizer, const char *input, size_t input_len,
                           char *output);

/* Replace an incomplete trailing sequence and reset the sanitizer. Writes at most
 * UTF8_SANITIZE_FLUSH_MAX bytes and returns the number written, or zero if nothing is pending. */
size_t utf8_sanitizer_flush(struct utf8_sanitizer *sanitizer, char *output);

/* Return an allocated, NUL-terminated copy with valid UTF-8 preserved and every NUL or malformed
 * byte replaced by U+FFFD. The caller frees the result. */
char *utf8_sanitize(const char *input, size_t input_len);

#endif /* HAX_TEXT_UTF8_SANITIZE_H */
