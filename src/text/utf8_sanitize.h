/* SPDX-License-Identifier: MIT */
#ifndef HAX_UTF8_SANITIZE_H
#define HAX_UTF8_SANITIZE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Stateful UTF-8 sanitizer for chunked input. Replaces every malformed
 * byte sequence with U+FFFD (the Unicode replacement character, encoded
 * as the three-byte sequence 0xEF 0xBF 0xBD) so the output is always
 * valid UTF-8.
 *
 * Strict per RFC 3629: rejects overlong encodings, surrogate code
 * points (U+D800..U+DFFF), and any code point above U+10FFFF. NUL
 * bytes (0x00) are also replaced with U+FFFD because they're useless
 * to the model and can cause issues in NUL-terminated downstream
 * pipelines (jansson, terminals, etc.).
 *
 * Designed for streaming: a multi-byte sequence split across feed()
 * calls is held in internal state and completed when the continuation
 * bytes arrive. The caller must call utf8_sanitize_flush() at end of
 * stream so any trailing partial sequence is flushed as U+FFFD instead
 * of silently dropped.
 *
 * Output expansion: in the worst case (every byte invalid) one input
 * byte produces three output bytes. Carrying that across chunk
 * boundaries: up to three bytes can be buffered between feed() calls,
 * so a single 1-byte feed can complete a 4-byte invalid sequence and
 * emit four U+FFFDs (12 bytes) plus replacements for whatever else
 * arrives. Use UTF8_SANITIZE_OUT_MAX(n) for the per-feed bound and
 * UTF8_SANITIZE_FLUSH_MAX for flush. Both are tight upper bounds.
 */
#define UTF8_SANITIZE_OUT_MAX(n) ((n) * 3 + 9)
#define UTF8_SANITIZE_FLUSH_MAX  9
struct utf8_sanitize {
    /* Bytes accumulated for an in-progress multi-byte sequence
     * (leader + continuations). buf_len < expected while still
     * waiting for more continuation bytes. */
    unsigned char buf[4];
    uint8_t buf_len;
    /* Total bytes the in-progress sequence will have (2, 3, or 4).
     * 0 means "not in a sequence". */
    uint8_t expected;
};

void utf8_sanitize_init(struct utf8_sanitize *s);

/* Feed n input bytes; write up to UTF8_SANITIZE_OUT_MAX(n) sanitized
 * bytes to out. Returns the number of bytes written. out must not
 * alias in. */
size_t utf8_sanitize_feed(struct utf8_sanitize *s, const char *in, size_t n, char *out);

/* Flush any pending in-progress sequence as U+FFFD. Writes up to
 * UTF8_SANITIZE_FLUSH_MAX bytes to out. Returns the number of bytes
 * written (0, 3, 6, or 9). Idempotent. Resets state so the same
 * struct can be reused. */
size_t utf8_sanitize_flush(struct utf8_sanitize *s, char *out);

/* One-shot convenience: convert an arbitrary byte range into a NUL-
 * free, valid UTF-8 string. Valid UTF-8 is preserved verbatim; NULs
 * and invalid sequences are replaced with U+FFFD. Returns a freshly-
 * allocated NUL-terminated buffer. Caller frees. Equivalent to
 * utf8_sanitize_init + feed + flush, with the buffer right-sized. */
char *sanitize_utf8(const char *data, size_t len);

#endif /* HAX_UTF8_SANITIZE_H */
