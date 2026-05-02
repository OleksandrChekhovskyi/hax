/* SPDX-License-Identifier: MIT */
#ifndef HAX_CTRL_STRIP_H
#define HAX_CTRL_STRIP_H

#include <stddef.h>

/*
 * Stateful sanitizer for terminal control bytes in tool output.
 *
 * Strips two kinds of hazard:
 *
 *   1. ANSI/ECMA-48 escape sequences:
 *      - CSI       (ESC [ ... final 0x40-0x7E), e.g. SGR colors "\x1b[31m"
 *      - OSC       (ESC ] ... BEL or ESC \), e.g. window titles
 *      - DCS/PM/APC (ESC P/^/_ ... ST)
 *      - Two-byte intermediate escapes (ESC <0x20-0x2F> <0x30-0x7E>)
 *      - Single-byte escapes (ESC <0x30-0x7E>)
 *
 *   2. Stray C0 control bytes that survive outside an escape:
 *      every byte in 0x00-0x1F is dropped *except* HT (\t) and LF (\n).
 *      DEL (0x7F) is dropped too. This catches FF (clear-screen on some
 *      terminals), VT (cursor jumps), SO/SI (alternate-charset shifts
 *      that leave the terminal stuck in line-drawing mode), bare CR
 *      (progress-bar overwrites — \r\n still yields \n because only the
 *      \r is dropped), BS, BEL, NUL, and the rest.
 *
 * 8-bit C1 controls (0x80-0x9F) are intentionally left alone because in
 * a UTF-8 stream those bytes are continuation bytes of multi-byte
 * characters; stripping them would corrupt valid UTF-8.
 *
 * In-flight escape sequences are aborted (and the cancelling byte
 * reconsumed) on LF, CAN (0x18), or SUB (0x1A). This bounds the damage
 * from a malformed or truncated escape — without it, an unterminated
 * "ESC [" would swallow arbitrary downstream text up to the next byte
 * in 0x40-0x7E, which can be many lines away in log-style output.
 *
 * Designed for chunked input — escape sequences split across feed()
 * calls are reassembled across the state held in `struct ctrl_strip`.
 * Output is never longer than input, so callers can size out buffers to
 * the input length. ctrl_strip_dup() is the one-shot convenience over a
 * NUL-terminated string.
 */
struct ctrl_strip {
    int state;
};

void ctrl_strip_init(struct ctrl_strip *s);

/* Feed n bytes of input; write at most n bytes of sanitized output to out
 * (which must not alias in). Returns the number of bytes written. */
size_t ctrl_strip_feed(struct ctrl_strip *s, const char *in, size_t n, char *out);

/* Sanitize a NUL-terminated string. Returns a freshly-allocated NUL-
 * terminated copy with control sequences removed. Caller frees. Note
 * that NUL bytes embedded in the input are dropped, but if you have a
 * NUL-terminated input string the terminator already bounds it, so the
 * embedded-NUL case really only applies to ctrl_strip_feed callers. */
char *ctrl_strip_dup(const char *s);

#endif /* HAX_CTRL_STRIP_H */
