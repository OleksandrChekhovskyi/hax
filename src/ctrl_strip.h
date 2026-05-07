/* SPDX-License-Identifier: MIT */
#ifndef HAX_CTRL_STRIP_H
#define HAX_CTRL_STRIP_H

#include <stddef.h>

/*
 * Stateful sanitizer for terminal control bytes in tool output.
 *
 * Drops three kinds of hazard:
 *
 *   1. Decorative / catastrophic CSI sequences (ESC [ … final 0x40-7E)
 *      whose final byte isn't on the layout-affecting allowlist —
 *      SGR colors "\x1b[31m", window titles, mouse modes, save-cursor,
 *      scroll-region setup, etc. These never reach term_lite.
 *
 *   2. OSC (ESC ] … BEL or ESC \), DCS / PM / APC (ESC P/^/_ … ST),
 *      single-byte and two-byte intermediate ESC sequences. All dropped.
 *
 *   3. Stray C0 control bytes outside an escape: every byte in 0x00-0x1F
 *      is dropped *except* HT (\t), LF (\n), CR (\r), and BS (\b). DEL
 *      (0x7F) is dropped too. This catches FF, VT, SO/SI, BEL, NUL, …
 *      CR and BS pass through because term_lite needs them for tty-style
 *      cursor motion.
 *
 * Layout-affecting CSI sequences pass through unchanged so term_lite
 * downstream can interpret them the way a real terminal would. The
 * allowlist of CSI finals is intentionally small:
 *
 *      A   CUU    cursor up
 *      B   CUD    cursor down
 *      C   CUF    cursor forward (column +)
 *      D   CUB    cursor back (column -)
 *      E   CNL    cursor next line (row +, col=0)
 *      F   CPL    cursor previous line (row -, col=0)
 *      G   CHA    cursor horizontal absolute (set column)
 *      H/f CUP    cursor position (set row;col)
 *      J   ED     erase in display
 *      K   EL     erase in line
 *      d   VPA    vertical position absolute (set row)
 *      s   SCOSC  save cursor position
 *      u   SCORC  restore cursor position
 *
 * Other CSIs (SGR colors, alt-screen, mouse modes, scroll regions,
 * insert/delete line, set/reset modes including DECTCEM cursor
 * visibility) are dropped — they don't affect what bytes land on
 * which row/col, which is all term_lite needs to model.
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
 * Output is never longer than input (allowlisted CSIs forward at most
 * the bytes that arrived, never expand), so callers can size out
 * buffers to the input length. ctrl_strip_dup() is the one-shot
 * convenience over a NUL-terminated string.
 */
struct ctrl_strip {
    int state;
    /* Accumulator for an in-flight CSI's params + intermediates. We
     * don't know whether to forward a CSI until its final byte arrives,
     * so the bytes between "ESC [" and the final live here. Capped at
     * 32 bytes — a CSI longer than that gets dropped wholesale (real
     * sequences are well under 16 bytes; an oversized one is almost
     * certainly malformed). */
    char csi_buf[32];
    size_t csi_len;
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
