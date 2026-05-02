/* SPDX-License-Identifier: MIT */
#include "utf8_sanitize.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

static const char REPLACEMENT[3] = {(char)0xEF, (char)0xBF, (char)0xBD}; /* U+FFFD */

static size_t emit_replacement(char *out)
{
    memcpy(out, REPLACEMENT, 3);
    return 3;
}

void utf8_sanitize_init(struct utf8_sanitize *s)
{
    s->buf_len = 0;
    s->expected = 0;
}

/* Validate a complete sequence held in s->buf. Returns 1 if it
 * encodes a valid scalar (rejects overlongs, surrogates, > U+10FFFF). */
static int sequence_is_valid(const struct utf8_sanitize *s)
{
    uint32_t cp;
    if (s->expected == 2) {
        cp = (uint32_t)(s->buf[0] & 0x1F) << 6 | (s->buf[1] & 0x3F);
        if (cp < 0x80)
            return 0; /* overlong */
    } else if (s->expected == 3) {
        cp = (uint32_t)(s->buf[0] & 0x0F) << 12 | (uint32_t)(s->buf[1] & 0x3F) << 6 |
             (s->buf[2] & 0x3F);
        if (cp < 0x800)
            return 0; /* overlong */
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return 0; /* surrogate */
    } else {          /* 4 */
        cp = (uint32_t)(s->buf[0] & 0x07) << 18 | (uint32_t)(s->buf[1] & 0x3F) << 12 |
             (uint32_t)(s->buf[2] & 0x3F) << 6 | (s->buf[3] & 0x3F);
        if (cp < 0x10000)
            return 0; /* overlong */
        if (cp > 0x10FFFF)
            return 0; /* out of range */
    }
    return 1;
}

/* Determine the total byte count for a leading byte. Returns 0 if c
 * is not a valid leading byte (continuation, overlong start, or > 4). */
static uint8_t leader_length(unsigned char c)
{
    if ((c & 0x80) == 0)
        return 1;
    if ((c & 0xC0) == 0x80)
        return 0; /* stray continuation */
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 0; /* 5/6-byte leaders illegal in modern UTF-8 */
}

size_t utf8_sanitize_feed(struct utf8_sanitize *s, const char *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];

        if (s->expected != 0) {
            /* Mid-sequence: expecting a continuation byte. */
            if ((c & 0xC0) == 0x80) {
                s->buf[s->buf_len++] = c;
                if (s->buf_len == s->expected) {
                    if (sequence_is_valid(s)) {
                        memcpy(out + o, s->buf, s->buf_len);
                        o += s->buf_len;
                    } else {
                        /* Invalid sequence (overlong, surrogate, > 10FFFF):
                         * one U+FFFD for the leader, plus one for each
                         * already-consumed continuation byte. Matches the
                         * stateless sanitize_utf8() in util.c so the
                         * display and history paths agree byte-for-byte. */
                        for (uint8_t i = 0; i < s->buf_len; i++)
                            o += emit_replacement(out + o);
                    }
                    s->buf_len = 0;
                    s->expected = 0;
                }
                continue;
            }
            /* Non-continuation byte arrived mid-sequence — replace each
             * already-consumed byte with U+FFFD, then reconsider c at
             * top-level (it may itself be a valid leader or ASCII). */
            for (uint8_t i = 0; i < s->buf_len; i++)
                o += emit_replacement(out + o);
            s->buf_len = 0;
            s->expected = 0;
            /* fall through to top-level handling of c */
        }

        if (c == 0) {
            /* NUL: replace, never pass through. */
            o += emit_replacement(out + o);
            continue;
        }
        if (c < 0x80) {
            out[o++] = (char)c;
            continue;
        }
        uint8_t need = leader_length(c);
        if (need == 0) {
            /* Stray continuation or illegal leader. */
            o += emit_replacement(out + o);
            continue;
        }
        /* need is 2, 3, or 4 — start collecting. */
        s->buf[0] = c;
        s->buf_len = 1;
        s->expected = need;
    }
    return o;
}

size_t utf8_sanitize_flush(struct utf8_sanitize *s, char *out)
{
    if (s->expected == 0)
        return 0;
    /* A partial sequence at end-of-stream — emit one U+FFFD per
     * already-consumed byte, matching the per-byte cadence used
     * inside utf8_sanitize_feed for invalid sequences. */
    size_t o = 0;
    for (uint8_t i = 0; i < s->buf_len; i++)
        o += emit_replacement(out + o);
    s->buf_len = 0;
    s->expected = 0;
    return o;
}

char *sanitize_utf8(const char *data, size_t len)
{
    /* Feed in chunks through a stack scratch so a clean input doesn't
     * pay a 3x peak allocation for no expansion. Scratch is sized for
     * the worst-case output of one CHUNK feed (UTF8_SANITIZE_OUT_MAX),
     * so buf grows roughly with the actual output size. */
    enum { CHUNK = 1361 }; /* 1361 * 3 + 9 = 4092 ≤ sizeof(scratch) */
    char scratch[UTF8_SANITIZE_OUT_MAX(CHUNK)];
    struct utf8_sanitize s;
    utf8_sanitize_init(&s);
    struct buf out;
    buf_init(&out);
    for (size_t i = 0; i < len;) {
        size_t take = len - i;
        if (take > CHUNK)
            take = CHUNK;
        size_t n = utf8_sanitize_feed(&s, data + i, take, scratch);
        buf_append(&out, scratch, n);
        i += take;
    }
    size_t tn = utf8_sanitize_flush(&s, scratch);
    buf_append(&out, scratch, tn);
    /* buf_steal returns a NUL-terminated copy; an empty buf gives "". */
    if (!out.data)
        return xstrdup("");
    return buf_steal(&out);
}
