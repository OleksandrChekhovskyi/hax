/* SPDX-License-Identifier: MIT */
#include "input_core.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "input.h"
#include "util.h"

/* ---------------- public API: alloc / free ---------------- */

struct input *input_new(void)
{
    struct input *in = xcalloc(1, sizeof(*in));
    in->buf = xmalloc(64);
    in->cap = 64;
    in->buf[0] = '\0';
    return in;
}

void input_free(struct input *in)
{
    if (!in)
        return;
    free(in->buf);
    free(in->draft);
    for (size_t i = 0; i < in->hist_n; i++)
        free(in->hist[i]);
    free(in->hist);
    free(in->persist_path);
    free(in);
}

/* ---------------- utf-8 helpers ---------------- */

int input_core_utf8_seq_len(unsigned char c)
{
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1; /* malformed leader — skip as one opaque byte */
}

/* Strictly validate a structurally complete UTF-8 sequence: reject
 * overlong encodings, UTF-16 surrogates, and codepoints past U+10FFFF.
 * The renderer (mbrtowc) and submit-time sanitizer reject these too,
 * substituting one '?' per byte; motions must agree, otherwise a single
 * Left/Right or Backspace skips multiple visible placeholders. */
static int utf8_seq_strict_valid(const unsigned char *s, int n)
{
    if (n == 1)
        return s[0] < 0x80;
    uint32_t cp;
    if (n == 2)
        cp = s[0] & 0x1F;
    else if (n == 3)
        cp = s[0] & 0x0F;
    else if (n == 4)
        cp = s[0] & 0x07;
    else
        return 0;
    for (int k = 1; k < n; k++) {
        if ((s[k] & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (s[k] & 0x3F);
    }
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

static size_t utf8_next(const char *s, size_t len, size_t i)
{
    if (i >= len)
        return len;
    int n = input_core_utf8_seq_len((unsigned char)s[i]);
    if ((size_t)n > len - i)
        return i + 1;
    if (!utf8_seq_strict_valid((const unsigned char *)s + i, n))
        return i + 1;
    return i + n;
}

static size_t utf8_prev(const char *s, size_t i)
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
    /* If the byte we landed on is itself a continuation, we ran out of
     * room without finding a leader — buffer is malformed; step one
     * byte. */
    unsigned char lead = (unsigned char)s[j];
    if ((lead & 0xC0) == 0x80)
        return i - 1;
    /* No continuation bytes seen → previous codepoint is single-byte
     * (ASCII or malformed leader); either way j is its position. */
    if (conts == 0)
        return j;
    /* Leader's claimed length must match the run we walked AND the
     * sequence must be strictly valid. Otherwise the renderer is
     * substituting one '?' per byte, and we must too. */
    if (input_core_utf8_seq_len(lead) != conts + 1)
        return i - 1;
    if (!utf8_seq_strict_valid((const unsigned char *)s + j, conts + 1))
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

int input_core_codepoint_width(const char *buf, size_t len, size_t i, size_t *consumed)
{
    if (i >= len) {
        *consumed = 0;
        return 0;
    }
    wchar_t wc;
    mbstate_t ps;
    memset(&ps, 0, sizeof(ps));
    size_t r = mbrtowc(&wc, buf + i, len - i, &ps);
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

int input_core_prompt_width(const char *s)
{
    int w = 0;
    size_t i = 0, len = strlen(s);
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n')
            break;
        if (c == 0x1b && i + 1 < len && s[i + 1] == '[') {
            i += 2;
            while (i < len && (unsigned char)s[i] < 0x40)
                i++;
            if (i < len)
                i++;
            continue;
        }
        size_t consumed;
        int cw = input_core_codepoint_width(s, len, i, &consumed);
        /* Substituted glyphs (controls, dangerous) render as 1 col;
         * mirror that here so a stray non-printable in a prompt can't
         * make the width go negative and corrupt continuation indent. */
        if (cw < 0)
            cw = 1;
        w += cw;
        i += consumed ? consumed : 1;
    }
    return w;
}

/* ---------------- buffer ops ---------------- */

static void buf_grow(struct input *in, size_t need)
{
    if (need <= in->cap)
        return;
    size_t cap = in->cap ? in->cap : 64;
    while (cap < need)
        cap *= 2;
    in->buf = xrealloc(in->buf, cap);
    in->cap = cap;
}

void input_core_buf_set(struct input *in, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    buf_grow(in, n + 1);
    if (n)
        memcpy(in->buf, s, n);
    in->buf[n] = '\0';
    in->len = n;
    in->cursor = n;
}

void input_core_buf_insert(struct input *in, const char *bytes, size_t n)
{
    if (n == 0)
        return;
    buf_grow(in, in->len + n + 1);
    memmove(in->buf + in->cursor + n, in->buf + in->cursor, in->len - in->cursor);
    memcpy(in->buf + in->cursor, bytes, n);
    in->len += n;
    in->cursor += n;
    in->buf[in->len] = '\0';
}

static void buf_erase(struct input *in, size_t pos, size_t n)
{
    if (pos >= in->len || n == 0)
        return;
    if (pos + n > in->len)
        n = in->len - pos;
    memmove(in->buf + pos, in->buf + pos + n, in->len - pos - n);
    in->len -= n;
    in->buf[in->len] = '\0';
    if (in->cursor > pos + n)
        in->cursor -= n;
    else if (in->cursor > pos)
        in->cursor = pos;
}

/* ---------------- motions / edits ---------------- */

size_t input_core_line_start(const struct input *in)
{
    size_t i = in->cursor;
    while (i > 0 && in->buf[i - 1] != '\n')
        i--;
    return i;
}

size_t input_core_line_end(const struct input *in)
{
    size_t i = in->cursor;
    while (i < in->len && in->buf[i] != '\n')
        i++;
    return i;
}

void input_core_move_left(struct input *in)
{
    if (in->cursor > 0)
        in->cursor = utf8_prev(in->buf, in->cursor);
}

void input_core_move_right(struct input *in)
{
    if (in->cursor < in->len)
        in->cursor = utf8_next(in->buf, in->len, in->cursor);
}

void input_core_delete_back(struct input *in)
{
    if (in->cursor == 0)
        return;
    size_t prev = utf8_prev(in->buf, in->cursor);
    buf_erase(in, prev, in->cursor - prev);
}

void input_core_delete_fwd(struct input *in)
{
    if (in->cursor >= in->len)
        return;
    size_t next = utf8_next(in->buf, in->len, in->cursor);
    buf_erase(in, in->cursor, next - in->cursor);
}

void input_core_kill_to_eol(struct input *in)
{
    size_t e = input_core_line_end(in);
    if (e == in->cursor && e < in->len && in->buf[e] == '\n')
        e++; /* on empty line, eat the newline so Ctrl-K joins lines */
    buf_erase(in, in->cursor, e - in->cursor);
}

void input_core_kill_to_bol(struct input *in)
{
    size_t b = input_core_line_start(in);
    buf_erase(in, b, in->cursor - b);
}

void input_core_kill_word_back(struct input *in)
{
    size_t i = in->cursor;
    while (i > 0 && isspace((unsigned char)in->buf[i - 1]))
        i--;
    while (i > 0 && !isspace((unsigned char)in->buf[i - 1]))
        i--;
    buf_erase(in, i, in->cursor - i);
}

/* ---------------- history ---------------- */

/* Append `line` to history without touching any persistence layer.
 * Erases any prior exact-match occurrences first (zsh
 * HIST_IGNORE_ALL_DUPS / bash HISTCONTROL=erasedups semantics) so a
 * recalled entry bumps to the top instead of duplicating — the same
 * canned prompts get reused constantly in a coding-agent REPL.
 *
 * Fast path: if `line` is already the most-recent entry, the erasedups
 * would self-cancel (erase idx hist_n-1, then re-append the same
 * string). Skip outright — saves the on-disk wrapper an append too. */
int input_core_history_add(struct input *in, const char *line)
{
    if (!line || !*line)
        return 0;
    if (in->hist_n > 0 && strcmp(in->hist[in->hist_n - 1], line) == 0)
        return 0;
    /* Erase prior exact matches. Walk back-to-front so indices stay
     * valid as we remove. */
    for (size_t i = in->hist_n; i > 0; i--) {
        if (strcmp(in->hist[i - 1], line) == 0) {
            free(in->hist[i - 1]);
            memmove(&in->hist[i - 1], &in->hist[i], (in->hist_n - i) * sizeof(char *));
            in->hist_n--;
        }
    }
    if (in->hist_n + 1 > in->hist_cap) {
        in->hist_cap = in->hist_cap ? in->hist_cap * 2 : 16;
        in->hist = xrealloc(in->hist, in->hist_cap * sizeof(char *));
    }
    in->hist[in->hist_n++] = xstrdup(line);
    if (in->hist_n > INPUT_CORE_HISTORY_MAX) {
        free(in->hist[0]);
        memmove(&in->hist[0], &in->hist[1], (in->hist_n - 1) * sizeof(char *));
        in->hist_n--;
    }
    return 1;
}

/* ---------------- history persistence (encode/decode) ---------------- */

/* Encode an entry for the on-disk one-line-per-record format: literal
 * backslash -> "\\", literal LF -> "\n". Caller frees. The result has
 * no trailing newline — the file writer adds one. */
char *input_core_history_encode(const char *s)
{
    if (!s)
        return xstrdup("");
    size_t n = strlen(s);
    char *out = xmalloc(n * 2 + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') {
            out[j++] = '\\';
            out[j++] = '\\';
        } else if (c == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    return out;
}

/* Decode `n` bytes of an encoded entry. Recognizes "\\" -> '\\' and
 * "\n" -> LF. An unrecognized escape (or trailing backslash) is
 * preserved verbatim — forward-compatible with future escape additions
 * and resilient to a hand-edited file. Caller frees. */
char *input_core_history_decode(const char *s, size_t n)
{
    char *out = xmalloc(n + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            char nx = s[i + 1];
            if (nx == '\\') {
                out[j++] = '\\';
                i++;
                continue;
            }
            if (nx == 'n') {
                out[j++] = '\n';
                i++;
                continue;
            }
        }
        out[j++] = s[i];
    }
    out[j] = '\0';
    return out;
}

/* History navigation convention:
 *
 * `hist_pos` ranges over [0, hist_n]. Values < hist_n point at a
 * recalled entry; the sentinel `hist_pos == hist_n` means "we're on
 * the user's current draft, not a history entry".
 *
 * On the first Up out of the draft, save the current buffer to
 * `draft`; subsequent Ups don't re-save (the user is browsing
 * history). On Down past the last entry, restore from `draft` and
 * free it. Edits made to a recalled entry are local to the buffer —
 * `hist[i]` strings are never mutated, so navigating away discards
 * those edits.
 */
static void hist_save_draft(struct input *in)
{
    free(in->draft);
    in->draft = xstrdup(in->buf);
}

static void hist_load(struct input *in, size_t pos)
{
    if (pos < in->hist_n) {
        input_core_buf_set(in, in->hist[pos]);
        in->hist_pos = pos;
    } else {
        input_core_buf_set(in, in->draft ? in->draft : "");
        free(in->draft);
        in->draft = NULL;
        in->hist_pos = in->hist_n;
    }
}

void input_core_history_prev(struct input *in)
{
    if (in->hist_n == 0)
        return;
    if (in->hist_pos == in->hist_n)
        hist_save_draft(in);
    if (in->hist_pos > 0)
        hist_load(in, in->hist_pos - 1);
}

void input_core_history_next(struct input *in)
{
    if (in->hist_pos >= in->hist_n)
        return;
    hist_load(in, in->hist_pos + 1);
}

/* ---------------- layout ---------------- */

void input_core_compute_layout(const char *buf, size_t len, size_t cursor, int prompt_w, int cols,
                               struct input_layout *out)
{
    int row = 0, col = prompt_w;
    int crow = -1, ccol = -1;
    size_t i = 0;
    for (;;) {
        if (i == cursor && crow < 0) {
            crow = row;
            ccol = col;
        }
        if (i >= len)
            break;
        unsigned char c = (unsigned char)buf[i];
        if (c == '\n') {
            row++;
            col = prompt_w;
            i++;
            continue;
        }
        if (c == '\t') {
            /* Soft-tab: every tab is exactly TAB_WIDTH columns wide,
             * regardless of current position. emit_safe_span in
             * input.c expands tabs to the same number of spaces, so
             * layout and rendering can't drift apart. */
            int w = INPUT_CORE_TAB_WIDTH;
            if (cols > 0 && col + w > cols) {
                row++;
                col = 0;
            }
            col += w;
            i++;
            continue;
        }
        size_t consumed;
        int w = input_core_codepoint_width(buf, len, i, &consumed);
        /* Non-printable (controls, malformed UTF-8) renders as a 1-col
         * substitute in emit_safe_span; mirror that here so layout and
         * rendering stay in sync. Combining marks legitimately have
         * w == 0 and must not advance the cursor. */
        if (w < 0)
            w = 1;
        if (cols > 0 && w > 0 && col + w > cols) {
            row++;
            col = 0;
        }
        col += w;
        i += consumed ? consumed : 1;
    }
    if (crow < 0) {
        crow = row;
        ccol = col;
    }
    out->cursor_row = crow;
    out->cursor_col = ccol;
    out->end_row = row;
    out->end_col = col;
    out->total_rows = row + 1;
}
