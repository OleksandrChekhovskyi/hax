/* SPDX-License-Identifier: MIT */
#ifndef HAX_UTF8_H
#define HAX_UTF8_H

#include <stddef.h>

/*
 * UTF-8 structural helpers and visual-cell measurement.
 *
 * Two kinds of routine live here:
 *
 *   - Pure structural primitives (utf8_seq_len, utf8_seq_valid,
 *     utf8_next, utf8_prev): byte-level iteration over codepoint
 *     boundaries. Locale-independent.
 *
 *   - Cell measurement (utf8_codepoint_cells): wcwidth-based visual
 *     width, with a curated filter that substitutes a single safe
 *     glyph for control bytes and "dangerous" invisibles (Trojan
 *     Source bidi vectors, ZWJ, BOM, etc.). Locale-dependent —
 *     requires LC_CTYPE = UTF-8 (see locale_init_utf8).
 *
 * For sanitization (replacing malformed input with U+FFFD before it
 * reaches downstream consumers), see utf8_sanitize.{c,h} — that's a
 * different concern with its own streaming state machine.
 *
 * All functions take a counted buffer (no NUL-terminator required)
 * and never allocate.
 */

/* Number of bytes in the UTF-8 sequence whose leader byte is `c`. Returns
 * 1 for ASCII (c < 0x80), 2/3/4 for valid leaders, and 1 for any byte
 * that isn't a valid leader (continuations, malformed leaders) — caller
 * treats those as one opaque byte. */
int utf8_seq_len(unsigned char c);

/* Strict validity check for a structurally complete UTF-8 sequence of
 * length `n` (1..4) at `s`. Rejects:
 *   - overlong encodings (e.g. 0xC0 0x80 for U+0000)
 *   - UTF-16 surrogates (U+D800..U+DFFF)
 *   - codepoints above U+10FFFF
 *   - sequences whose continuation bytes aren't 10xxxxxx
 *   - n outside [1, 4]
 *
 * Caller is expected to first call utf8_seq_len on s[0] to determine n
 * and to verify there are at least n bytes available; this function
 * does NOT bounds-check `s` past byte n-1. */
int utf8_seq_valid(const char *s, int n);

/* Advance one codepoint forward from byte index `i` in s[0..len). On a
 * well-formed leader with all continuation bytes available and the full
 * sequence strictly valid, returns i + utf8_seq_len(s[i]). On any
 * problem (truncated, bad continuation bytes, invalid codepoint),
 * returns i + 1 — single-byte step over the offending byte, mirroring
 * the renderer's "one substitute per byte" policy.
 *
 * Returns `len` when i >= len (idempotent at end-of-buffer). Always
 * makes forward progress when i < len. */
size_t utf8_next(const char *s, size_t len, size_t i);

/* Step one codepoint backward from byte index `i`, returning the byte
 * position of the previous codepoint's leader. Walks back over up to 3
 * continuation bytes, then validates the sequence; on malformed input
 * (lone continuation, leader/length mismatch, sequence not strictly
 * valid) steps exactly one byte, mirroring utf8_next's policy.
 *
 * Returns 0 when i == 0 (idempotent at start-of-buffer). Always makes
 * backward progress when i > 0. Doesn't take a `len` parameter — the
 * walk lives entirely within [0, i). */
size_t utf8_prev(const char *s, size_t i);

/* Visual cell width of the codepoint at s[i..]. Writes the codepoint's
 * UTF-8 byte length to *consumed (always >= 1 when i < len). Returns:
 *   < 0 — non-printable: C0/C1 controls, DEL, malformed UTF-8,
 *         embedded NUL, or one of a curated list of "dangerous"
 *         codepoints (Trojan Source bidi vectors, ZWJ, BOM, format
 *         characters that hide or rearrange content). Caller
 *         substitutes a single safe glyph rather than emit the raw
 *         bytes.
 *   == 0 — combining mark or zero-width joiner; rides on prior glyph.
 *   > 0 — printable codepoint occupying that many columns.
 *
 * Tab and newline are NOT special-cased — callers handle them.
 *
 * Locale-dependent: requires the LC_CTYPE locale to be UTF-8 for
 * multi-byte codepoints to decode correctly. Call locale_init_utf8()
 * once at program startup. Tests must setlocale(LC_CTYPE, "C.UTF-8")
 * (or equivalent) before exercising multi-byte input. */
int utf8_codepoint_cells(const char *s, size_t len, size_t i, size_t *consumed);

/* Streaming UTF-8 decoder. Consumes bytes one at a time and yields
 * complete codepoints with their visual cell width — for callers that
 * receive bytes incrementally (markdown wrap, etc.) and can't measure
 * cells until a multi-byte sequence is complete.
 *
 * Initialize by zeroing the struct (or call utf8_stream_reset). */
struct utf8_stream {
    unsigned char buf[4];
    unsigned char have;
};

/* Feed one byte. Returns 1 when a complete unit is ready: *out points
 * into the stream's internal buffer for *out_n bytes, with *out_cells
 * visual columns. The caller must consume *out before the next call —
 * subsequent feeds may clobber the buffer.
 *
 * For valid sequences *out_cells is the wcwidth-style measurement
 * (0 for combining marks, >=1 for printables; -1 results from
 * utf8_codepoint_cells are clamped to 1 cell so non-printables render
 * as a single-cell substitute glyph).
 *
 * Returns 0 when more bytes are needed (mid-sequence). On a malformed
 * byte (bad leader, invalid continuation, overlong, surrogate) the
 * decoder emits the buffered bytes as one opaque "malformed run" with
 * one cell per byte — no recovery, no byte loss; the terminal renders
 * each as a replacement glyph. */
int utf8_stream_byte(struct utf8_stream *s, unsigned char c, const char **out, size_t *out_n,
                     int *out_cells);

/* Drain any incomplete trailing sequence. Returns 1 with the buffered
 * bytes (1..3) if the stream ended mid-codepoint, 0 if the buffer is
 * already empty. After a successful call the stream is reset. Use at
 * end-of-stream so a truncated multi-byte sequence isn't lost. */
int utf8_stream_flush(struct utf8_stream *s, const char **out, size_t *out_n, int *out_cells);

void utf8_stream_reset(struct utf8_stream *s);

#endif /* HAX_UTF8_H */
