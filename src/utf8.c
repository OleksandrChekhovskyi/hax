/* SPDX-License-Identifier: MIT */
#include "utf8.h"

#include <stdint.h>
#include <string.h>
#include <wchar.h>

int utf8_seq_len(unsigned char c)
{
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1; /* continuation byte or malformed leader */
}

int utf8_seq_valid(const char *s, int n)
{
    const unsigned char *u = (const unsigned char *)s;
    if (n < 1 || n > 4)
        return 0;
    /* Leader byte must match the claimed length: a continuation byte
     * (0x80-0xBF) or a leader for a different length must reject
     * outright. Without this gate, callers passing arbitrary (s, n)
     * could "validate" e.g. "\xBF\xBF" as a 2-byte sequence even
     * though 0xBF is never a leader. */
    if (utf8_seq_len(u[0]) != n)
        return 0;
    if (n == 1)
        return u[0] < 0x80;
    uint32_t cp;
    if (n == 2)
        cp = u[0] & 0x1F;
    else if (n == 3)
        cp = u[0] & 0x0F;
    else
        cp = u[0] & 0x07;
    for (int k = 1; k < n; k++) {
        if ((u[k] & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (u[k] & 0x3F);
    }
    /* Reject overlongs: each length has a minimum codepoint. */
    if (n == 2 && cp < 0x80)
        return 0;
    if (n == 3 && cp < 0x800)
        return 0;
    if (n == 4 && cp < 0x10000)
        return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0;
    if (cp > 0x10FFFF)
        return 0;
    return 1;
}

size_t utf8_next(const char *s, size_t len, size_t i)
{
    if (i >= len)
        return len;
    int n = utf8_seq_len((unsigned char)s[i]);
    if ((size_t)n > len - i)
        return i + 1;
    if (!utf8_seq_valid(s + i, n))
        return i + 1;
    return i + n;
}

size_t utf8_prev(const char *s, size_t i)
{
    if (i == 0)
        return 0;
    /* Walk back over up to 3 continuation bytes. */
    size_t j = i - 1;
    int conts = 0;
    while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80 && conts < 3) {
        j--;
        conts++;
    }
    unsigned char lead = (unsigned char)s[j];
    if ((lead & 0xC0) == 0x80)
        return i - 1;
    if (conts == 0)
        return j;
    if (utf8_seq_len(lead) != conts + 1)
        return i - 1;
    if (!utf8_seq_valid(s + j, conts + 1))
        return i - 1;
    return j;
}

/* Codepoints that are technically printable per wcwidth (returning 0
 * or 1 depending on libc) but that we always want to substitute, since
 * they can rearrange or hide content in the rendered terminal output.
 * Covers the Trojan Source bidi vectors and the common
 * Default_Ignorable_Code_Point invisibles. Variation selectors
 * (U+FE00..FE0F) are intentionally kept since they're needed for
 * emoji presentation. Substituting ZWJ does break emoji ZWJ sequences
 * during edit; we accept that — the buffer keeps the original bytes
 * and the model receives the unmodified content. */
static int codepoint_is_dangerous(wchar_t wc)
{
    if (wc == 0x00AD) /* soft hyphen */
        return 1;
    if (wc == 0x034F) /* combining grapheme joiner */
        return 1;
    if (wc == 0x061C) /* arabic letter mark (bidi) */
        return 1;
    if (wc == 0x115F || wc == 0x1160) /* Hangul choseong / jungseong filler */
        return 1;
    if (wc == 0x180E) /* Mongolian vowel separator */
        return 1;
    if (wc >= 0x200B && wc <= 0x200F) /* ZWSP, ZWNJ, ZWJ, LRM, RLM */
        return 1;
    if (wc >= 0x202A && wc <= 0x202E) /* bidi overrides (Trojan Source) */
        return 1;
    if (wc == 0x2028 || wc == 0x2029) /* line / paragraph separators */
        return 1;
    if (wc >= 0x2060 && wc <= 0x206F) /* word joiner, invisible math, isolates, deprecated */
        return 1;
    if (wc == 0x3164) /* Hangul filler */
        return 1;
    if (wc == 0xFEFF) /* BOM / ZWNBSP */
        return 1;
    if (wc == 0xFFA0) /* halfwidth Hangul filler */
        return 1;
    if (wc >= 0xFFF9 && wc <= 0xFFFB) /* interlinear annotation marks */
        return 1;
    if (wc >= 0xE0000 && wc <= 0xE007F) /* language tag characters */
        return 1;
    return 0;
}

int utf8_codepoint_cells(const char *s, size_t len, size_t i, size_t *consumed)
{
    if (i >= len) {
        *consumed = 0;
        return 0;
    }
    wchar_t wc;
    mbstate_t ps;
    memset(&ps, 0, sizeof(ps));
    size_t r = mbrtowc(&wc, s + i, len - i, &ps);
    if (r == (size_t)-1 || r == (size_t)-2 || r == 0) {
        /* Malformed UTF-8 or embedded NUL — caller substitutes. */
        *consumed = 1;
        return -1;
    }
    *consumed = r;
    if (codepoint_is_dangerous(wc))
        return -1;
    return wcwidth(wc); /* -1 covers C0/C1 controls, DEL, format chars */
}
