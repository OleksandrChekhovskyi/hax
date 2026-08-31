/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "harness.h"
#include "system/locale.h"
#include "text/utf8.h"

static void test_sequence_length_ascii(void)
{
    EXPECT(utf8_sequence_length('a') == 1);
    EXPECT(utf8_sequence_length(0x00) == 1);
    EXPECT(utf8_sequence_length(0x7F) == 1);
}

static void test_sequence_length_multibyte_leaders(void)
{
    EXPECT(utf8_sequence_length(0xC2) == 2);
    EXPECT(utf8_sequence_length(0xDF) == 2);
    EXPECT(utf8_sequence_length(0xE0) == 3);
    EXPECT(utf8_sequence_length(0xEF) == 3);
    EXPECT(utf8_sequence_length(0xF0) == 4);
    EXPECT(utf8_sequence_length(0xF4) == 4);
}

static void test_sequence_length_malformed(void)
{
    EXPECT(utf8_sequence_length(0x80) == 1);
    EXPECT(utf8_sequence_length(0xBF) == 1);
    EXPECT(utf8_sequence_length(0xC0) == 1);
    EXPECT(utf8_sequence_length(0xC1) == 1);
    EXPECT(utf8_sequence_length(0xF5) == 1);
    EXPECT(utf8_sequence_length(0xF8) == 1);
    EXPECT(utf8_sequence_length(0xFF) == 1);
}

static void test_sequence_valid_ascii(void)
{
    EXPECT(utf8_sequence_is_valid("a", 1) == 1);
    EXPECT(utf8_sequence_is_valid("\x00", 1) == 1);
    EXPECT(utf8_sequence_is_valid("\x80", 1) == 0);
}

static void test_sequence_valid_two_byte(void)
{
    /* "é" — U+00E9 = C3 A9. */
    EXPECT(utf8_sequence_is_valid("\xC3\xA9", 2) == 1);
    EXPECT(utf8_sequence_is_valid("\xC3\x00", 2) == 0);
    EXPECT(utf8_sequence_is_valid("\xC3\xC0", 2) == 0);
}

static void test_sequence_valid_three_byte(void)
{
    /* "—" — U+2014 = E2 80 94. */
    EXPECT(utf8_sequence_is_valid("\xE2\x80\x94", 3) == 1);
}

static void test_sequence_valid_four_byte(void)
{
    /* "🦀" — U+1F980 = F0 9F A6 80. */
    EXPECT(utf8_sequence_is_valid("\xF0\x9F\xA6\x80", 4) == 1);
    EXPECT(utf8_sequence_is_valid("\xF4\x8F\xBF\xBF", 4) == 1); /* U+10FFFF */
}

static void test_sequence_valid_rejects_overlong(void)
{
    /* C0 80 would encode U+0000 as 2 bytes — overlong, must reject. */
    EXPECT(utf8_sequence_is_valid("\xC0\x80", 2) == 0);
    /* E0 80 80 → U+0000 as 3 bytes. */
    EXPECT(utf8_sequence_is_valid("\xE0\x80\x80", 3) == 0);
    /* F0 80 80 80 → U+0000 as 4 bytes. */
    EXPECT(utf8_sequence_is_valid("\xF0\x80\x80\x80", 4) == 0);
    /* E0 9F BF → U+07FF as 3 bytes (valid as 2). */
    EXPECT(utf8_sequence_is_valid("\xE0\x9F\xBF", 3) == 0);
}

static void test_sequence_valid_rejects_surrogate(void)
{
    /* U+D800 = ED A0 80 — UTF-16 surrogate, illegal in UTF-8. */
    EXPECT(utf8_sequence_is_valid("\xED\xA0\x80", 3) == 0);
    /* U+DFFF = ED BF BF. */
    EXPECT(utf8_sequence_is_valid("\xED\xBF\xBF", 3) == 0);
}

static void test_sequence_valid_rejects_above_max(void)
{
    /* U+110000 = F4 90 80 80 — past Unicode's max. */
    EXPECT(utf8_sequence_is_valid("\xF4\x90\x80\x80", 4) == 0);
}

static void test_sequence_valid_rejects_bad_length(void)
{
    EXPECT(utf8_sequence_is_valid("a", 0) == 0);
    EXPECT(utf8_sequence_is_valid("a", 5) == 0);
}

static void test_sequence_valid_rejects_continuation_leader(void)
{
    EXPECT(utf8_sequence_is_valid("\xBF\xBF", 2) == 0);
    EXPECT(utf8_sequence_is_valid("\xBF\x80\x80", 3) == 0);
    EXPECT(utf8_sequence_is_valid("\x80\xBF\xBF", 3) == 0);
}

static void test_sequence_valid_rejects_length_mismatch(void)
{
    EXPECT(utf8_sequence_is_valid("\xC3\xA9\xA9", 3) == 0);
    EXPECT(utf8_sequence_is_valid("\xE2\x80", 2) == 0);
}

static void test_buffer_validity(void)
{
    EXPECT(utf8_is_valid("", 0));
    EXPECT(utf8_is_valid("plain", 5));
    EXPECT(utf8_is_valid("a\0b", 3));
    EXPECT(utf8_is_valid("\xC3\xA9\xE2\x80\x94", 5));
    EXPECT(!utf8_is_valid("\xC3", 1));
    EXPECT(!utf8_is_valid("\xC0\x80", 2));
    EXPECT(!utf8_is_valid("\xED\xA0\x80", 3));
}

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
    const char *s = "ab";
    EXPECT(utf8_next(s, 2, 2) == 2);
    EXPECT(utf8_next(s, 2, 100) == 2);
}

static void test_next_truncated_sequence(void)
{
    const char *s = "\xE2\x80";
    EXPECT(utf8_next(s, 2, 0) == 1); /* step 1 byte */
}

static void test_next_invalid_continuation(void)
{
    const char *s = "\xC3z";
    EXPECT(utf8_next(s, 2, 0) == 1);
}

static void test_next_lone_continuation(void)
{
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

static void test_prev_ascii(void)
{
    const char *s = "abc";
    EXPECT(utf8_prev(s, 3) == 2);
    EXPECT(utf8_prev(s, 2) == 1);
    EXPECT(utf8_prev(s, 1) == 0);
}

static void test_prev_at_start(void)
{
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

static void test_cells_ascii(void)
{
    size_t consumed = 0;
    EXPECT(utf8_codepoint_cells("a", 1, 0, &consumed) == 1);
    EXPECT(consumed == 1);
}

static void test_cells_at_end(void)
{
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
    size_t consumed = 0;
    EXPECT(utf8_codepoint_cells("\x01", 1, 0, &consumed) == -1);
    EXPECT(consumed == 1);
}

static void test_cells_unsafe_bidi(void)
{
    /* U+202E (RIGHT-TO-LEFT OVERRIDE, Trojan Source vector) = E2 80 AE.
     * wcwidth might say 0 or 1; we always substitute (-1). */
    size_t consumed = 0;
    EXPECT(utf8_codepoint_cells("\xE2\x80\xAE", 3, 0, &consumed) == -1);
    EXPECT(consumed == 3);
}

static void test_cells_malformed(void)
{
    size_t consumed = 0;
    EXPECT(utf8_codepoint_cells("\xC3", 1, 0, &consumed) == -1);
    EXPECT(consumed == 1);
}

static void test_cells_combining_mark(void)
{
    /* U+0301 COMBINING ACUTE ACCENT — wcwidth returns 0 (rides on
     * the prior glyph). It is not an unsafe format character, so its zero width is preserved. */
    size_t consumed = 0;
    EXPECT(utf8_codepoint_cells("\xCC\x81", 2, 0, &consumed) == 0);
    EXPECT(consumed == 2);
}

static void test_stream_ascii(void)
{
    struct utf8_cell_stream stream = {0};
    const char *output;
    size_t output_len;
    int cells;

    for (int byte = 'a'; byte <= 'c'; byte++) {
        EXPECT(utf8_cell_stream_feed(&stream, (unsigned char)byte, &output, &output_len, &cells) ==
               1);
        EXPECT(output_len == 1 && output[0] == (char)byte && cells == 1);
    }
}

static void test_stream_two_byte_sequence(void)
{
    /* "é" = C3 A9. The first byte must NOT emit; the second completes
     * the sequence with 1 cell. */
    struct utf8_cell_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_cell_stream_feed(&s, 0xC3, &out, &n, &cells) == 0);
    EXPECT(utf8_cell_stream_feed(&s, 0xA9, &out, &n, &cells) == 1);
    EXPECT(n == 2 && memcmp(out, "\xC3\xA9", 2) == 0);
    EXPECT(cells == 1);
}

static void test_stream_four_byte_sequence(void)
{
    /* "🦀" = F0 9F A6 80, 4 bytes, 2 cells. */
    struct utf8_cell_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_cell_stream_feed(&s, 0xF0, &out, &n, &cells) == 0);
    EXPECT(utf8_cell_stream_feed(&s, 0x9F, &out, &n, &cells) == 0);
    EXPECT(utf8_cell_stream_feed(&s, 0xA6, &out, &n, &cells) == 0);
    EXPECT(utf8_cell_stream_feed(&s, 0x80, &out, &n, &cells) == 1);
    EXPECT(n == 4 && memcmp(out, "\xF0\x9F\xA6\x80", 4) == 0);
    EXPECT(cells == 2);
}

static void test_stream_combining_zero_width(void)
{
    /* Combining acute accent U+0301 = CC 81, wcwidth returns 0. */
    struct utf8_cell_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_cell_stream_feed(&s, 0xCC, &out, &n, &cells) == 0);
    EXPECT(utf8_cell_stream_feed(&s, 0x81, &out, &n, &cells) == 1);
    EXPECT(cells == 0);
}

static void test_stream_control_one_cell_substitute(void)
{
    /* Control byte 0x01 — utf8_codepoint_cells returns -1; stream
     * substitutes 1 cell so the wrap layer can move past. */
    struct utf8_cell_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_cell_stream_feed(&s, 0x01, &out, &n, &cells) == 1);
    EXPECT(n == 1 && cells == 1);
}

static void test_stream_unsafe_one_cell(void)
{
    /* U+202E bidi override = E2 80 AE — three bytes, marked unsafe,
     * stream emits the original bytes with 1-cell substitution. */
    struct utf8_cell_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_cell_stream_feed(&s, 0xE2, &out, &n, &cells) == 0);
    EXPECT(utf8_cell_stream_feed(&s, 0x80, &out, &n, &cells) == 0);
    EXPECT(utf8_cell_stream_feed(&s, 0xAE, &out, &n, &cells) == 1);
    EXPECT(n == 3 && memcmp(out, "\xE2\x80\xAE", 3) == 0);
    EXPECT(cells == 1);
}

static void test_stream_bad_continuation_dumps_run(void)
{
    /* C3 followed by 'a' (not a continuation byte) — the leader and
     * the offender flush together as one malformed run with one cell
     * per byte. The next-fed byte starts a fresh sequence. */
    struct utf8_cell_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_cell_stream_feed(&s, 0xC3, &out, &n, &cells) == 0);
    EXPECT(utf8_cell_stream_feed(&s, 'a', &out, &n, &cells) == 1);
    EXPECT(n == 2 && cells == 2);
    EXPECT(memcmp(out,
                  "\xC3"
                  "a",
                  2) == 0);
}

static void test_stream_lone_continuation(void)
{
    /* Lone 0x80 — utf8_sequence_length says 1, so it emits as a single-byte
     * malformed unit with 1-cell substitution. */
    struct utf8_cell_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_cell_stream_feed(&s, 0x80, &out, &n, &cells) == 1);
    EXPECT(n == 1 && cells == 1);
}

static void test_stream_malformed_leaders_emit_immediately(void)
{
    struct utf8_cell_stream stream = {0};
    const char *output;
    size_t output_len;
    int cells;

    EXPECT(utf8_cell_stream_feed(&stream, 0xC0, &output, &output_len, &cells) == 1);
    EXPECT(output_len == 1 && (unsigned char)output[0] == 0xC0 && cells == 1);
    EXPECT(utf8_cell_stream_feed(&stream, 0xF5, &output, &output_len, &cells) == 1);
    EXPECT(output_len == 1 && (unsigned char)output[0] == 0xF5 && cells == 1);
}

static void test_stream_invalid_scalar_uses_one_cell_per_byte(void)
{
    struct utf8_cell_stream stream = {0};
    const char *output;
    size_t output_len;
    int cells;

    EXPECT(utf8_cell_stream_feed(&stream, 0xED, &output, &output_len, &cells) == 0);
    EXPECT(utf8_cell_stream_feed(&stream, 0xA0, &output, &output_len, &cells) == 0);
    EXPECT(utf8_cell_stream_feed(&stream, 0x80, &output, &output_len, &cells) == 1);
    EXPECT(output_len == 3 && cells == 3);
}

static void test_stream_flush_drains_partial(void)
{
    /* Truncated 3-byte sequence at end-of-stream. flush emits the
     * buffered bytes with one cell per byte. */
    struct utf8_cell_stream s = {0};
    const char *out;
    size_t n;
    int cells;
    EXPECT(utf8_cell_stream_feed(&s, 0xE2, &out, &n, &cells) == 0);
    EXPECT(utf8_cell_stream_feed(&s, 0x80, &out, &n, &cells) == 0);
    EXPECT(utf8_cell_stream_flush(&s, &out, &n, &cells) == 1);
    EXPECT(n == 2 && cells == 2);
    EXPECT(utf8_cell_stream_flush(&s, &out, &n, &cells) == 0);
}

static void test_stream_reset_discards_pending_bytes(void)
{
    struct utf8_cell_stream stream = {0};
    const char *output;
    size_t output_len;
    int cells;

    EXPECT(utf8_cell_stream_feed(&stream, 0xE2, &output, &output_len, &cells) == 0);
    utf8_cell_stream_reset(&stream);
    EXPECT(utf8_cell_stream_feed(&stream, 'a', &output, &output_len, &cells) == 1);
    EXPECT(output_len == 1 && output[0] == 'a' && cells == 1);
}

int main(void)
{
    /* utf8_codepoint_cells uses mbrtowc + wcwidth which need a UTF-8
     * LC_CTYPE for multi-byte decoding. */
    locale_init_utf8();

    test_sequence_length_ascii();
    test_sequence_length_multibyte_leaders();
    test_sequence_length_malformed();

    test_sequence_valid_ascii();
    test_sequence_valid_two_byte();
    test_sequence_valid_three_byte();
    test_sequence_valid_four_byte();
    test_sequence_valid_rejects_overlong();
    test_sequence_valid_rejects_surrogate();
    test_sequence_valid_rejects_above_max();
    test_sequence_valid_rejects_bad_length();
    test_sequence_valid_rejects_continuation_leader();
    test_sequence_valid_rejects_length_mismatch();
    test_buffer_validity();

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
    test_cells_unsafe_bidi();
    test_cells_malformed();
    test_cells_combining_mark();

    test_stream_ascii();
    test_stream_two_byte_sequence();
    test_stream_four_byte_sequence();
    test_stream_combining_zero_width();
    test_stream_control_one_cell_substitute();
    test_stream_unsafe_one_cell();
    test_stream_bad_continuation_dumps_run();
    test_stream_lone_continuation();
    test_stream_malformed_leaders_emit_immediately();
    test_stream_invalid_scalar_uses_one_cell_per_byte();
    test_stream_flush_drains_partial();
    test_stream_reset_discards_pending_bytes();

    T_REPORT();
}
