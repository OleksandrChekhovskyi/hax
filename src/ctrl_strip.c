/* SPDX-License-Identifier: MIT */
#include "ctrl_strip.h"

#include <string.h>

#include "util.h"

/* State machine. Names mirror the ECMA-48 categories so the transitions
 * read against the spec rather than against ad-hoc abbreviations. */
enum {
    S_NORMAL = 0,
    S_ESC,       /* saw ESC; next byte selects sequence kind          */
    S_CSI,       /* ESC [ — params/intermediates until final 0x40-7E  */
    S_OSC,       /* ESC ] — until BEL or ST (ESC \)                   */
    S_OSC_ESC,   /* OSC saw ESC, expecting \ to complete ST           */
    S_STR,       /* ESC P/^/_ (DCS/PM/APC) — until ST                 */
    S_STR_ESC,   /* STR saw ESC, expecting \ to complete ST           */
    S_ESC_INTER, /* ESC followed by an intermediate; awaiting final   */
};

/* Bytes that pass through S_NORMAL untouched: printables (>= 0x20, also
 * includes the high half 0x80-0xFF which is UTF-8 territory), plus HT
 * (0x09) and LF (0x0A). Everything else in the C0 range and DEL (0x7F)
 * is dropped. ESC (0x1B) is handled by the state-transition arm
 * separately, so it never reaches this predicate. */
static int is_passthrough(unsigned char c)
{
    if (c == '\t' || c == '\n')
        return 1;
    if (c < 0x20)
        return 0;
    if (c == 0x7f)
        return 0;
    return 1;
}

/* Bytes that abort an in-flight escape sequence (CSI/OSC/DCS/PM/APC/...).
 * CAN (0x18) and SUB (0x1A) are explicitly defined as cancel by ECMA-48
 * §5.3 and observed in xterm. LF is included pragmatically: a malformed
 * or truncated escape would otherwise swallow arbitrary text up to the
 * next final byte (catastrophic for log-style streaming output where
 * the next final byte may be many lines away). On abort we reconsume
 * the byte under S_NORMAL so a real LF still emits as LF, while CAN /
 * SUB get dropped as ordinary C0 controls. */
static int is_abort(unsigned char c)
{
    return c == 0x0a || c == 0x18 || c == 0x1a;
}

void ctrl_strip_init(struct ctrl_strip *s)
{
    s->state = S_NORMAL;
}

size_t ctrl_strip_feed(struct ctrl_strip *s, const char *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (s->state) {
        case S_NORMAL:
            if (c == 0x1b)
                s->state = S_ESC;
            else if (is_passthrough(c))
                out[o++] = (char)c;
            /* else: silently drop a stray C0 control or DEL */
            break;
        case S_ESC:
            if (c == '[') {
                s->state = S_CSI;
            } else if (c == ']') {
                s->state = S_OSC;
            } else if (c == 'P' || c == '^' || c == '_') {
                s->state = S_STR;
            } else if (c >= 0x20 && c <= 0x2f) {
                s->state = S_ESC_INTER;
            } else if (c >= 0x30 && c <= 0x7e) {
                /* Single-byte ESC sequence (ESC c, ESC =, ESC 7, …). */
                s->state = S_NORMAL;
            } else {
                /* Malformed — drop the ESC and reconsume the byte under
                 * the normal-state rules so real text isn't swallowed. */
                s->state = S_NORMAL;
                i--;
            }
            break;
        case S_CSI:
            /* params (0x30-0x3F) and intermediates (0x20-0x2F) absorbed
             * until the final byte (0x40-0x7E). LF/CAN/SUB cancel the
             * sequence so a malformed or truncated CSI can't swallow
             * arbitrary downstream text. */
            if (is_abort(c)) {
                s->state = S_NORMAL;
                i--;
            } else if (c >= 0x40 && c <= 0x7e) {
                s->state = S_NORMAL;
            }
            break;
        case S_OSC:
            if (is_abort(c)) {
                s->state = S_NORMAL;
                i--;
            } else if (c == 0x07) {
                s->state = S_NORMAL; /* BEL terminator */
            } else if (c == 0x1b) {
                s->state = S_OSC_ESC; /* possible ST */
            }
            break;
        case S_OSC_ESC:
            if (c == '\\') {
                s->state = S_NORMAL;
            } else if (is_abort(c)) {
                s->state = S_NORMAL;
                i--;
            } else {
                /* Not a real ST — fall back to OSC and reconsume this
                 * byte so an embedded ESC followed by content keeps the
                 * string alive. */
                s->state = S_OSC;
                i--;
            }
            break;
        case S_STR:
            if (is_abort(c)) {
                s->state = S_NORMAL;
                i--;
            } else if (c == 0x1b) {
                s->state = S_STR_ESC;
            }
            break;
        case S_STR_ESC:
            if (c == '\\') {
                s->state = S_NORMAL;
            } else if (is_abort(c)) {
                s->state = S_NORMAL;
                i--;
            } else {
                s->state = S_STR;
                i--;
            }
            break;
        case S_ESC_INTER:
            if (is_abort(c)) {
                s->state = S_NORMAL;
                i--;
            } else if (c >= 0x30 && c <= 0x7e) {
                s->state = S_NORMAL;
            }
            /* else stay — additional intermediate bytes are legal */
            break;
        }
    }
    return o;
}

char *ctrl_strip_dup(const char *s)
{
    size_t n = strlen(s);
    char *out = xmalloc(n + 1);
    struct ctrl_strip st;
    ctrl_strip_init(&st);
    size_t w = ctrl_strip_feed(&st, s, n, out);
    out[w] = '\0';
    return out;
}
