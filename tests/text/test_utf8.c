/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "harness.h"
#include "util.h"
#include "text/utf8.h"

/* ---------- utf8_seq_len ---------- */

static void test_seq_len_ascii(void)
{
    EXPECT(utf8_seq_len('a') == 1);
    EXPECT(utf8_seq_len(0x00) == 1);
    EXPECT(utf8_seq_len(0x7F) == 1);
}

static void test_seq_len_multibyte_leaders(void)
{
    EXPECT(utf8_seq_len(0xC3) == 2); /* é, ñ, ... */
    EXPECT(utf8_seq_len(0xE2) == 3); /* —, …, box-drawing */
    EXPECT(utf8_seq_len(0xF0) == 4); /* emoji, U+10000+ */
}

static void test_seq_len_malformed(void)
{
    EXPECT(utf8_seq_len(0x80) == 1); /* continuation byte */
    EXPECT(utf8_seq_len(0xBF) == 1); /* continuation byte */
    EXPECT(utf8_seq_len(0xF8) == 1); /* 5-byte leader (invalid in modern UTF-8) */
    EXPECT(utf8_seq_len(0xFF) == 1); /* invalid in any UTF-8 */
}

/* ---------- utf8_seq_valid ---------- */

static void test_valid_ascii(void)
{
    EXPECT(utf8_seq_valid("a", 1) == 1);
    EXPECT(utf8_seq_valid("\x00", 1) == 1);
    /* 0x80 is not a valid 1-byte sequence (it's a continuation byte). */
    EXPECT(utf8_seq_valid("\x80", 1) == 0);
}

static void test_valid_2byte(void)
{
    /* "é" — U+00E9 = C3 A9. */
    EXPECT(utf8_seq_valid("\xC3\xA9", 2) == 1);
    /* Bad continuation byte. */
    EXPECT(utf8_seq_valid("\xC3\x00", 2) == 0);
    EXPECT(utf8_seq_valid("\xC3\xC0", 2) == 0);
}

static void test_valid_3byte(void)
{
    /* "—" — U+2014 = E2 80 94. */
    EXPECT(utf8_seq_valid("\xE2\x80\x94", 3) == 1);
}

static void test_valid_4byte(void)
{
    /* "🦀" — U+1F980 = F0 9F A6 80. */
    EXPECT(utf8_seq_valid("\xF0\x9F\xA6\x80", 4) == 1);
}

static void test_valid_overlong_rejected(void)
{
    /* C0 80 would encode U+0000 as 2 bytes — overlong, must reject. */
    EXPECT(utf8_seq_valid("\xC0\x80", 2) == 0);
    /* E0 80 80 → U+0000 as 3 bytes. */
    EXPECT(utf8_seq_valid("\xE0\x80\x80", 3) == 0);
    /* F0 80 80 80 → U+0000 as 4 bytes. */
    EXPECT(utf8_seq_valid("\xF0\x80\x80\x80", 4) == 0);
    /* E0 9F BF → U+07FF as 3 bytes (valid as 2). */
    EXPECT(utf8_seq_valid("\xE0\x9F\xBF", 3) == 0);
}

static void test_valid_surrogate_rejected(void)
{
    /* U+D800 = ED A0 80 — UTF-16 surrogate, illegal in UTF-8. */
    EXPECT(utf8_seq_valid("\xED\xA0\x80", 3) == 0);
    /* U+DFFF = ED BF BF. */
    EXPECT(utf8_seq_valid("\xED\xBF\xBF", 3) == 0);
}

static void test_valid_above_max(void)
{
    /* U+110000 = F4 90 80 80 — past Unicode's max. */
    EXPECT(utf8_seq_valid("\xF4\x90\x80\x80", 4) == 0);
}

static void test_valid_bad_n(void)
{
    EXPECT(utf8_seq_valid("a", 0) == 0);
    EXPECT(utf8_seq_valid("a", 5) == 0);
}

static void test_valid_rejects_continuation_as_leader(void)
{
    /* 0xBF is a continuation byte, never a valid leader of any
     * length. Strict validation must reject — without the leader/
     * length gate, the bit-extraction code would happily decode
     * (0xBF & 0x1F) as the start of a 2-byte codepoint. */
    EXPECT(utf8_seq_valid("\xBF\xBF", 2) == 0);
    EXPECT(utf8_seq_valid("\xBF\x80\x80", 3) == 0);
    EXPECT(utf8_seq_valid("\x80\xBF\xBF", 3) == 0);
}

static void test_valid_rejects_leader_length_mismatch(void)
{
    /* 0xC3 is a 2-byte leader; calling with n=3 must reject even
     * if the next two bytes are valid continuations. */
    EXPECT(utf8_seq_valid("\xC3\xA9\xA9", 3) == 0);
    /* 0xE2 is a 3-byte leader; n=2 must reject. */
    EXPECT(utf8_seq_valid("\xE2\x80", 2) == 0);
}

/* ---------- utf8_next ---------- */

static void test_next_ascii(void)
{
    const char *s = "abc";
    EXPECT(utf8_next(s, 3, 0) == 1);
    EXPECT(utf8_next(s, 3, 1) == 2);
    EXPECT(utf8_next(s, 3, 2) == 3);
}

static void test_next_multibyte(void)
{
    /* "café": c(0) a(1) f(2) é=C3 A9(3,4). */
    const char *s = "caf\xC3\xA9";
    EXPECT(utf8_next(s, 5, 0) == 1);
    EXPECT(utf8_next(s, 5, 3) == 5); /* é → skip 2 bytes */
}

static void test_next_at_end(void)
{
    /* Idempotent at len. */
    const char *s = "ab";
    EXPECT(utf8_next(s, 2, 2) == 2);
    EXPECT(utf8_next(s, 2, 100) == 2);
}

static void test_next_truncated_sequence(void)
{
    /* 3-byte leader with only 1 continuation byte available — truncated. */
    const char *s = "\xE2\x80";
    EXPECT(utf8_next(s, 2, 0) == 1); /* step 1 byte */
}

static void test_next_invalid_continuation(void)
{
    /* 2-byte leader followed by ASCII (not a continuation byte). */
    const char *s = "\xC3z";
    EXPECT(utf8_next(s, 2, 0) == 1);
}

static void test_next_lone_continuation(void)
{
    /* Continuation byte without a leader — single-byte step. */
    const char *s = "\x80\x80";
    EXPECT(utf8_next(s, 2, 0) == 1);
}

static void test_next_overlong_steps_one(void)
{
    /* Overlong encoding mirrors the renderer's "one substitute per
     * byte" policy: utf8_next returns +1, not +2. */
    const char *s = "\xC0\x80";
    EXPECT(utf8_next(s, 2, 0) == 1);
}

/* ---------- utf8_prev ---------- */

static void test_prev_ascii(void)
{
    const char *s = "abc";
    EXPECT(utf8_prev(s, 3) == 2);
    EXPECT(utf8_prev(s, 2) == 1);
    EXPECT(utf8_prev(s, 1) == 0);
}

static void test_prev_at_start(void)
{
    /* Idempotent at i=0. */
    EXPECT(utf8_prev("ab", 0) == 0);
}

static void test_prev_multibyte(void)
{
    /* "café": c(0) a(1) f(2) é=C3 A9(3,4). Stepping back from 5
     * lands at 3 (start of é). */
    const char *s = "caf\xC3\xA9";
    EXPECT(utf8_prev(s, 5) == 3);
    /* Stepping back from 3 lands at 2 (start of f). */
    EXPECT(utf8_prev(s, 3) == 2);
}

static void test_prev_lone_continuation(void)
{
    /* Continuation bytes with no leader — single-byte step. */
    const char *s = "\x80\x80";
    EXPECT(utf8_prev(s, 2) == 1);
}

static void test_prev_overlong_steps_one(void)
{
    /* Overlong C0 80 isn't a valid codepoint; mirrors utf8_next's
     * "one byte per malformed sequence" policy in reverse. */
    const char *s = "\xC0\x80";
    EXPECT(utf8_prev(s, 2) == 1);
}

static void test_prev_leader_length_mismatch(void)
{
    /* 2-byte leader (0xC3) followed by another leader (0xC3): walking
     * back from position 2 finds 0 continuation bytes, so the
     * "previous codepoint" is the single byte at position 1. */
    const char *s = "\xC3\xC3";
    EXPECT(utf8_prev(s, 2) == 1);
}

/* ---------- utf8_codepoint_cells ---------- */

static void test_cells_ascii(void)
{
    size_t consumed = 0;
    EXPECT(utf8_codepoint_cells("a", 1, 0, &consumed) == 1);
    EXPECT(consumed == 1);
}

static void test_cells_at_end(void)
{
    /* i >= len: 0 cells, 0 consumed. */
    size_t consumed = 99;
    EXPECT(utf8_codepoint_cells("a", 1, 1, &consumed) == 0);
    EXPECT(consumed == 0);
}

static void test_cells_multibyte_one_cell(void)
{
    /* "é" = 2 bytes, 1 cell. */
    size_t consumed = 0;
    EXPECT(utf8_codepoint_cells("\xC3\xA9", 2, 0, &consumed) == 1);
    EXPECT(consumed == 2);
}

static void test_cells_emoji_two_cells(void)
{
    /* "🦀" = 4 bytes, 2 cells (wide East-Asian-style codepoint). */
    size_t consumed = 0;
    EXPECT(utf8_codepoint_cells("\xF0\x9F\xA6\x80", 4, 0, &consumed) == 2);
    EXPECT(consumed == 4);
}

static void test_cells_control_byte(void)
{
    /* Control byte: returns -1, consumed=1. Caller substitutes. */
    size_t consumed = 0;
    EXPECT(utf8_codepoint_cells("\x01", 1, 0, &consumed) == -1);
    EXPECT(consumed == 1);
}

static void test_cells_dangerous_bidi(void)
{
    /* U+202E (RIGHT-TO-LEFT OVERRIDE, Trojan Source vector) = E2 80 AE.
     * wcwidth might say 0 or 1; we always substitute (-1). */
    size_t consumed = 0;
    EXPECT(utf8_codepoint_cells("\xE2\x80\xAE", 3, 0, &consumed) == -1);
    EXPECT(consumed == 3);
}

static void test_cells_malformed(void)
{
    /* Truncated 2-byte sequence: returns -1, consumed=1 (single-byte step). */
    size_t consumed = 0;
    EXPECT(utf8_codepoint_cells("\xC3", 1, 0, &consumed) == -1);
    EXPECT(consumed == 1);
}

static void test_cells_combining_mark(void)
{
    /* U+0301 COMBINING ACUTE ACCENT — wcwidth returns 0 (rides on
     * the prior glyph). Not in the dangerous list, so passes through
     * as a real 0. UTF-8: CC 81 (2 bytes). */
    size_t consumed = 0;
    EXPECT(utf8_codepoint_cells("\xCC\x81", 2, 0, &consumed) == 0);
    EXPECT(consumed == 2);
}

/* ---------- utf8_stream ---------- */

/* Drive the streaming decoder over a byte string and collect every
 * emitted unit into a flat buffer (concatenated bytes) plus a parallel
 * cells array. Lets tests assert both the byte run AND the cell budget
 * a wrap layer would see. */
struct stream_capture {
    struct buf bytes;
    int cells[64];
    int n;
};

static void feed_all(struct utf8_stream *s, const char *input, size_t len, struct stream_capture *c)
{
    for (size_t i = 0; i < len; i++) {
        const char *out;
        size_t n;
        int cells;
        if (utf8_stream_byte(s, (unsigned char)input[i], &out, &n, &cells)) {
            buf_append(&c->bytes, out, n);
            c->cells[c->n++] = cells;
        }
    }
}

static void test_stream_ascii(void)
{
    struct utf8_stream s = {0};
    struct stream_capture c = {0};
    buf_init(&c.bytes);
    feed_all(&s, "abc", 3, &c);
    EXPECT(c.n == 3);
    EXPECT(c.bytes.len == 3 && memcmp(c.bytes.data, "abc", 3) == 0);
    EXPECT(c.cells[0] == 1 && c.cells[1] == 1 && c.cells[2] == 1);
    buf_free(&c.bytes);
}

static void test_stream_multibyte_2_assembled(void)
{
    /* "é" = C3 A9. The first byte must NOT emit; the second completes
     * the sequence with 1 cell. */
    struct utf8_stream s = {0};
    struct stream_capture c = {0};
    buf_init(&c.bytes);
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_stream_byte(&s, 0xC3, &out, &n, &cells) == 0);
    EXPECT(utf8_stream_byte(&s, 0xA9, &out, &n, &cells) == 1);
    EXPECT(n == 2 && memcmp(out, "\xC3\xA9", 2) == 0);
    EXPECT(cells == 1);
    buf_free(&c.bytes);
}

static void test_stream_emoji_4_bytes(void)
{
    /* "🦀" = F0 9F A6 80, 4 bytes, 2 cells. */
    struct utf8_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_stream_byte(&s, 0xF0, &out, &n, &cells) == 0);
    EXPECT(utf8_stream_byte(&s, 0x9F, &out, &n, &cells) == 0);
    EXPECT(utf8_stream_byte(&s, 0xA6, &out, &n, &cells) == 0);
    EXPECT(utf8_stream_byte(&s, 0x80, &out, &n, &cells) == 1);
    EXPECT(n == 4 && memcmp(out, "\xF0\x9F\xA6\x80", 4) == 0);
    EXPECT(cells == 2);
}

static void test_stream_combining_zero_width(void)
{
    /* Combining acute accent U+0301 = CC 81, wcwidth returns 0. */
    struct utf8_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_stream_byte(&s, 0xCC, &out, &n, &cells) == 0);
    EXPECT(utf8_stream_byte(&s, 0x81, &out, &n, &cells) == 1);
    EXPECT(cells == 0);
}

static void test_stream_control_one_cell_substitute(void)
{
    /* Control byte 0x01 — utf8_codepoint_cells returns -1; stream
     * substitutes 1 cell so the wrap layer can move past. */
    struct utf8_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_stream_byte(&s, 0x01, &out, &n, &cells) == 1);
    EXPECT(n == 1 && cells == 1);
}

static void test_stream_dangerous_one_cell(void)
{
    /* U+202E bidi override = E2 80 AE — three bytes, marked dangerous,
     * stream emits the original bytes with 1-cell substitution. */
    struct utf8_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_stream_byte(&s, 0xE2, &out, &n, &cells) == 0);
    EXPECT(utf8_stream_byte(&s, 0x80, &out, &n, &cells) == 0);
    EXPECT(utf8_stream_byte(&s, 0xAE, &out, &n, &cells) == 1);
    EXPECT(n == 3 && memcmp(out, "\xE2\x80\xAE", 3) == 0);
    EXPECT(cells == 1);
}

static void test_stream_bad_continuation_dumps_run(void)
{
    /* C3 followed by 'a' (not a continuation byte) — the leader and
     * the offender flush together as one malformed run with one cell
     * per byte. The next-fed byte starts a fresh sequence. */
    struct utf8_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_stream_byte(&s, 0xC3, &out, &n, &cells) == 0);
    EXPECT(utf8_stream_byte(&s, 'a', &out, &n, &cells) == 1);
    EXPECT(n == 2 && cells == 2);
    EXPECT(memcmp(out,
                  "\xC3"
                  "a",
                  2) == 0);
}

static void test_stream_lone_continuation(void)
{
    /* Lone 0x80 — utf8_seq_len says 1, so it emits as a single-byte
     * malformed unit with 1-cell substitution. */
    struct utf8_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_stream_byte(&s, 0x80, &out, &n, &cells) == 1);
    EXPECT(n == 1 && cells == 1);
}

static void test_stream_overlong_dumps_run(void)
{
    /* C0 80 (overlong U+0000) is structurally complete but
     * utf8_seq_valid rejects it. Stream reports a malformed run. */
    struct utf8_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_stream_byte(&s, 0xC0, &out, &n, &cells) == 0);
    EXPECT(utf8_stream_byte(&s, 0x80, &out, &n, &cells) == 1);
    EXPECT(n == 2 && cells == 2);
}

static void test_stream_flush_drains_partial(void)
{
    /* Truncated 3-byte sequence at end-of-stream. flush emits the
     * buffered bytes with one cell per byte. */
    struct utf8_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_stream_byte(&s, 0xE2, &out, &n, &cells) == 0);
    EXPECT(utf8_stream_byte(&s, 0x80, &out, &n, &cells) == 0);
    EXPECT(utf8_stream_flush(&s, &out, &n, &cells) == 1);
    EXPECT(n == 2 && cells == 2);
    EXPECT(utf8_stream_flush(&s, &out, &n, &cells) == 0);
}

static void test_stream_reset(void)
{
    struct utf8_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    utf8_stream_byte(&s, 0xE2, &out, &n, &cells);
    EXPECT(s.have != 0);
    utf8_stream_reset(&s);
    EXPECT(s.have == 0);
}

int main(void)
{
    /* utf8_codepoint_cells uses mbrtowc + wcwidth which need a UTF-8
     * LC_CTYPE for multi-byte decoding. */
    locale_init_utf8();

    test_seq_len_ascii();
    test_seq_len_multibyte_leaders();
    test_seq_len_malformed();

    test_valid_ascii();
    test_valid_2byte();
    test_valid_3byte();
    test_valid_4byte();
    test_valid_overlong_rejected();
    test_valid_surrogate_rejected();
    test_valid_above_max();
    test_valid_bad_n();
    test_valid_rejects_continuation_as_leader();
    test_valid_rejects_leader_length_mismatch();

    test_next_ascii();
    test_next_multibyte();
    test_next_at_end();
    test_next_truncated_sequence();
    test_next_invalid_continuation();
    test_next_lone_continuation();
    test_next_overlong_steps_one();

    test_prev_ascii();
    test_prev_at_start();
    test_prev_multibyte();
    test_prev_lone_continuation();
    test_prev_overlong_steps_one();
    test_prev_leader_length_mismatch();

    test_cells_ascii();
    test_cells_at_end();
    test_cells_multibyte_one_cell();
    test_cells_emoji_two_cells();
    test_cells_control_byte();
    test_cells_dangerous_bidi();
    test_cells_malformed();
    test_cells_combining_mark();

    test_stream_ascii();
    test_stream_multibyte_2_assembled();
    test_stream_emoji_4_bytes();
    test_stream_combining_zero_width();
    test_stream_control_one_cell_substitute();
    test_stream_dangerous_one_cell();
    test_stream_bad_continuation_dumps_run();
    test_stream_lone_continuation();
    test_stream_overlong_dumps_run();
    test_stream_flush_drains_partial();
    test_stream_reset();

    T_REPORT();
}
