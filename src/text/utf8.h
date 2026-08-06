/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_UTF8_H
#define HAX_TEXT_UTF8_H

#include <stddef.h>

/* Return the encoded length for an ASCII byte or a possible UTF-8 leader. Malformed leaders and
 * continuation bytes are treated as one-byte units. */
size_t utf8_sequence_length(unsigned char byte);

/* Return whether bytes[0..length) is exactly one Unicode scalar value encoded as UTF-8. The length
 * must be between one and four, and the caller must provide that many readable bytes. */
int utf8_sequence_is_valid(const char *bytes, size_t length);

/* Return whether the entire counted buffer is valid UTF-8. */
int utf8_is_valid(const char *bytes, size_t length);

/* Move by one valid codepoint. Malformed or truncated input is traversed one byte at a time.
 * utf8_next returns length at or beyond the end; utf8_prev returns zero at the start. */
size_t utf8_next(const char *bytes, size_t length, size_t offset);
size_t utf8_prev(const char *bytes, size_t offset);

/* Measure the codepoint at offset and write its byte length to codepoint_len. Returns zero at the
 * end, a positive terminal-cell width for printable codepoints, and -1 for malformed input,
 * controls, or format characters that could hide or rearrange terminal content. Tab and newline
 * have no special handling. Requires a UTF-8 LC_CTYPE locale for non-ASCII input. */
int utf8_codepoint_cells(const char *bytes, size_t length, size_t offset, size_t *codepoint_len);

/* Incremental codepoint assembly for cell accounting. Zero-initialize or reset before use. */
struct utf8_cell_stream {
    unsigned char pending[4];
    unsigned char pending_len;
};

void utf8_cell_stream_reset(struct utf8_cell_stream *stream);

/* Feed one byte. A return value of one exposes a borrowed unit through bytes and length; consume it
 * before the next call. Valid units use their measured width. Malformed bytes are preserved and
 * assigned one cell each. A byte that interrupts a sequence joins that malformed unit rather than
 * being reconsidered. Controls and unsafe format characters are assigned one cell. */
int utf8_cell_stream_feed(struct utf8_cell_stream *stream, unsigned char byte, const char **bytes,
                          size_t *length, int *cells);

/* Emit an incomplete tail as one cell per byte. Returns zero when nothing is pending. */
int utf8_cell_stream_flush(struct utf8_cell_stream *stream, const char **bytes, size_t *length,
                           int *cells);

#endif /* HAX_TEXT_UTF8_H */
