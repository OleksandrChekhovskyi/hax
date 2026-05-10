/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "text/utf8_sanitize.h"

#define FFFD "\xEF\xBF\xBD"

/* Convenience: full one-shot sanitize through feed + flush. */
static size_t one_shot(const char *in, size_t n, char *out)
{
    struct utf8_sanitize s;
    utf8_sanitize_init(&s);
    size_t o = utf8_sanitize_feed(&s, in, n, out);
    o += utf8_sanitize_flush(&s, out + o);
    return o;
}

static void test_ascii_passthrough(void)
{
    char out[64];
    size_t n = one_shot("hello world\n", 12, out);
    EXPECT(n == 12);
    EXPECT(memcmp(out, "hello world\n", 12) == 0);
}

static void test_valid_two_three_four_byte(void)
{
    /* "é" (U+00E9, C3 A9), "中" (U+4E2D, E4 B8 AD), "𝓐" (U+1D4D0, F0 9D 93 90). */
    const char in[] = "\xC3\xA9\xE4\xB8\xAD\xF0\x9D\x93\x90";
    char out[32];
    size_t n = one_shot(in, sizeof(in) - 1, out);
    EXPECT(n == sizeof(in) - 1);
    EXPECT(memcmp(out, in, n) == 0);
}

static void test_lone_continuation(void)
{
    /* 0xA0 alone is a stray continuation — replace with U+FFFD. */
    char out[16];
    size_t n = one_shot("\xA0", 1, out);
    EXPECT(n == 3);
    EXPECT(memcmp(out, FFFD, 3) == 0);
}

static void test_lone_high_byte(void)
{
    /* 0xFF — illegal anywhere in UTF-8. */
    char out[16];
    size_t n = one_shot("\xFF", 1, out);
    EXPECT(n == 3);
    EXPECT(memcmp(out, FFFD, 3) == 0);
}

static void test_truncated_at_eof(void)
{
    /* Leader of a 3-byte sequence with no continuation — flush should
     * emit one U+FFFD. */
    char out[16];
    size_t n = one_shot("\xE4", 1, out);
    EXPECT(n == 3);
    EXPECT(memcmp(out, FFFD, 3) == 0);
}

static void test_overlong_two_byte(void)
{
    /* C0 80 would encode U+0000 — overlong, must replace. */
    char out[16];
    size_t n = one_shot("\xC0\x80", 2, out);
    /* Both bytes flagged: leader is invalid (overlong), continuation
     * also gets replaced as a stray. */
    EXPECT(n == 6);
    EXPECT(memcmp(out, FFFD FFFD, 6) == 0);
}

static void test_surrogate_rejected(void)
{
    /* ED A0 80 = U+D800 (high surrogate, illegal in UTF-8). One FFFD
     * per byte, matching util.c's stateless sanitize_utf8. */
    char out[16];
    size_t n = one_shot("\xED\xA0\x80", 3, out);
    EXPECT(n == 9);
    EXPECT(memcmp(out, FFFD FFFD FFFD, 9) == 0);
}

static void test_above_max_codepoint(void)
{
    /* F4 90 80 80 = U+110000, one past the maximum. */
    char out[16];
    size_t n = one_shot("\xF4\x90\x80\x80", 4, out);
    EXPECT(n == 12);
    EXPECT(memcmp(out, FFFD FFFD FFFD FFFD, 12) == 0);
}

static void test_nul_replaced(void)
{
    char in[] = "a\0b";
    char out[16];
    size_t n = one_shot(in, 3, out);
    EXPECT(n == 5);
    EXPECT(memcmp(out, "a" FFFD "b", 5) == 0);
}

static void test_chunk_split_two_byte(void)
{
    /* "é" (C3 A9) split across feeds — must come out intact. */
    struct utf8_sanitize s;
    utf8_sanitize_init(&s);
    char out[16];
    size_t n = utf8_sanitize_feed(&s, "\xC3", 1, out);
    EXPECT(n == 0); /* leader buffered, nothing emitted yet */
    n = utf8_sanitize_feed(&s, "\xA9", 1, out);
    EXPECT(n == 2);
    EXPECT(memcmp(out, "\xC3\xA9", 2) == 0);
    EXPECT(utf8_sanitize_flush(&s, out) == 0);
}

static void test_chunk_split_three_byte(void)
{
    /* "中" (E4 B8 AD) split across two feeds at every boundary. */
    const char *boundaries[] = {"\xE4|\xB8\xAD", "\xE4\xB8|\xAD"};
    for (size_t k = 0; k < 2; k++) {
        struct utf8_sanitize s;
        utf8_sanitize_init(&s);
        const char *split = strchr(boundaries[k], '|');
        size_t a = (size_t)(split - boundaries[k]);
        size_t b = strlen(boundaries[k]) - a - 1;
        char out[16];
        size_t total = utf8_sanitize_feed(&s, boundaries[k], a, out);
        total += utf8_sanitize_feed(&s, split + 1, b, out + total);
        total += utf8_sanitize_flush(&s, out + total);
        EXPECT(total == 3);
        EXPECT(memcmp(out, "\xE4\xB8\xAD", 3) == 0);
    }
}

static void test_invalid_continuation_aborts_sequence(void)
{
    /* Leader followed by a non-continuation byte: must replace what we
     * had and reconsider the new byte at the top. */
    struct utf8_sanitize s;
    utf8_sanitize_init(&s);
    char out[16];
    /* C3 ('é' leader) then 'A' (ASCII). C3 → U+FFFD, then 'A'. */
    size_t n = utf8_sanitize_feed(&s,
                                  "\xC3"
                                  "A",
                                  2, out);
    n += utf8_sanitize_flush(&s, out + n);
    EXPECT(n == 4);
    EXPECT(memcmp(out, FFFD "A", 4) == 0);
}

static void test_flush_idempotent(void)
{
    struct utf8_sanitize s;
    utf8_sanitize_init(&s);
    char out[16];
    EXPECT(utf8_sanitize_flush(&s, out) == 0);
    size_t n = utf8_sanitize_feed(&s, "\xE4", 1, out);
    EXPECT(n == 0);
    n = utf8_sanitize_flush(&s, out);
    EXPECT(n == 3);
    /* Second flush is a no-op now that state was reset. */
    EXPECT(utf8_sanitize_flush(&s, out) == 0);
}

static void test_chunk_split_completes_invalid_seq(void)
{
    /* Regression: F4 90 80 (3 bytes buffered as a 4-byte sequence)
     * followed by a single 0x80 completing it. The completion is
     * invalid (U+110000 > U+10FFFF) and emits FOUR U+FFFDs (12 bytes)
     * from a 1-byte feed. With the documented bound this requires
     * UTF8_SANITIZE_OUT_MAX(1) >= 12 — the implementation must not
     * blow past it. */
    struct utf8_sanitize s;
    utf8_sanitize_init(&s);
    char out[UTF8_SANITIZE_OUT_MAX(8)];
    size_t n = utf8_sanitize_feed(&s, "\xF4\x90\x80", 3, out);
    EXPECT(n == 0); /* all three bytes buffered */
    n = utf8_sanitize_feed(&s, "\x80", 1, out);
    EXPECT(n == 12);
    EXPECT(memcmp(out, FFFD FFFD FFFD FFFD, 12) == 0);
    EXPECT(utf8_sanitize_flush(&s, out) == 0);
}

static void test_chunk_split_aborts_with_pending_buffer(void)
{
    /* Even more pathological: 3 bytes buffered, then a non-continuation
     * byte arrives. The buffered bytes get 3 U+FFFDs (9 bytes) and the
     * new byte is reconsumed — if it itself starts a fresh sequence we
     * could buffer again, or emit ASCII. Bound is OUT_MAX(1) = 12. */
    struct utf8_sanitize s;
    utf8_sanitize_init(&s);
    char out[UTF8_SANITIZE_OUT_MAX(8)];
    /* F4 90 80 buffers 3 bytes (4-byte sequence). */
    size_t n = utf8_sanitize_feed(&s, "\xF4\x90\x80", 3, out);
    EXPECT(n == 0);
    /* 'A' is non-continuation: emit 3 FFFDs for buffer + 1 for 'A'. */
    n = utf8_sanitize_feed(&s, "A", 1, out);
    EXPECT(n == 10);
    EXPECT(memcmp(out, FFFD FFFD FFFD "A", 10) == 0);
}

static void test_flush_max_three_buffered(void)
{
    /* Three buffered bytes flushed at EOF must produce exactly three
     * U+FFFDs and fit under UTF8_SANITIZE_FLUSH_MAX. */
    struct utf8_sanitize s;
    utf8_sanitize_init(&s);
    char out[UTF8_SANITIZE_FLUSH_MAX];
    size_t n = utf8_sanitize_feed(&s, "\xF0\x9F\x8E", 3, out);
    EXPECT(n == 0);
    n = utf8_sanitize_flush(&s, out);
    EXPECT(n == 9);
    EXPECT(memcmp(out, FFFD FFFD FFFD, 9) == 0);
}

static void test_byte_by_byte_equivalence(void)
{
    /* Feeding one byte at a time must produce the same output as one
     * shot, for both valid and malformed input. */
    const char in[] = "ok \xC3\xA9 \xE4\xB8\xAD trailing \xFF garbage \xC3";
    size_t in_len = sizeof(in) - 1;

    char one[64];
    size_t one_n = one_shot(in, in_len, one);

    struct utf8_sanitize s;
    utf8_sanitize_init(&s);
    char piece[64];
    size_t piece_n = 0;
    for (size_t i = 0; i < in_len; i++)
        piece_n += utf8_sanitize_feed(&s, in + i, 1, piece + piece_n);
    piece_n += utf8_sanitize_flush(&s, piece + piece_n);

    EXPECT(piece_n == one_n);
    EXPECT(memcmp(piece, one, piece_n) == 0);
}

/* ---------- one-shot wrapper (sanitize_utf8) ---------- */

static void test_wrapper_ascii(void)
{
    char *out = sanitize_utf8("hello", 5);
    EXPECT_STR_EQ(out, "hello");
    free(out);
}

static void test_wrapper_empty(void)
{
    /* Even on empty input the wrapper must return a freeable, NUL-
     * terminated buffer. */
    char *out = sanitize_utf8("", 0);
    EXPECT(out != NULL);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_wrapper_nul_byte_replaced(void)
{
    char *out = sanitize_utf8("a\0b", 3);
    EXPECT_MEM_EQ(out, strlen(out), "a" FFFD "b", 5);
    free(out);
}

static void test_wrapper_valid_multibyte_preserved(void)
{
    /* "é€🎉" = C3 A9 | E2 82 AC | F0 9F 8E 89 */
    const char in[] = "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x8E\x89";
    char *out = sanitize_utf8(in, sizeof(in) - 1);
    EXPECT_STR_EQ(out, in);
    free(out);
}

static void test_wrapper_overlong_two_byte_nul(void)
{
    /* C0 80 — overlong NUL. One U+FFFD per byte. */
    char *out = sanitize_utf8("\xC0\x80", 2);
    EXPECT_STR_EQ(out, FFFD FFFD);
    free(out);
}

static void test_wrapper_overlong_three_byte(void)
{
    /* E0 80 80 — overlong encoding of U+0000 */
    char *out = sanitize_utf8("\xE0\x80\x80", 3);
    EXPECT_STR_EQ(out, FFFD FFFD FFFD);
    free(out);
}

static void test_wrapper_surrogate_rejected(void)
{
    /* ED A0 80 = U+D800, high surrogate */
    char *out = sanitize_utf8("\xED\xA0\x80", 3);
    EXPECT_STR_EQ(out, FFFD FFFD FFFD);
    free(out);
}

static void test_wrapper_above_max_codepoint(void)
{
    /* F4 90 80 80 = U+110000, beyond U+10FFFF */
    char *out = sanitize_utf8("\xF4\x90\x80\x80", 4);
    EXPECT_STR_EQ(out, FFFD FFFD FFFD FFFD);
    free(out);
}

static void test_wrapper_truncated_tail(void)
{
    /* 0xC2 starts a 2-byte sequence but there's no continuation byte. */
    char *out = sanitize_utf8("\xC2", 1);
    EXPECT_STR_EQ(out, FFFD);
    free(out);
}

static void test_wrapper_invalid_continuation(void)
{
    /* C2 20 — 0x20 is ASCII, not a continuation byte. */
    char *out = sanitize_utf8("\xC2 ", 2);
    EXPECT_STR_EQ(out, FFFD " ");
    free(out);
}

static void test_wrapper_invalid_leading_byte(void)
{
    /* 0xFF is not a valid UTF-8 leading byte at all. */
    char *out = sanitize_utf8("x\xFFy", 3);
    EXPECT_STR_EQ(out, "x" FFFD "y");
    free(out);
}

int main(void)
{
    test_ascii_passthrough();
    test_valid_two_three_four_byte();
    test_lone_continuation();
    test_lone_high_byte();
    test_truncated_at_eof();
    test_overlong_two_byte();
    test_surrogate_rejected();
    test_above_max_codepoint();
    test_nul_replaced();
    test_chunk_split_two_byte();
    test_chunk_split_three_byte();
    test_invalid_continuation_aborts_sequence();
    test_chunk_split_completes_invalid_seq();
    test_chunk_split_aborts_with_pending_buffer();
    test_flush_max_three_buffered();
    test_flush_idempotent();
    test_byte_by_byte_equivalence();
    test_wrapper_ascii();
    test_wrapper_empty();
    test_wrapper_nul_byte_replaced();
    test_wrapper_valid_multibyte_preserved();
    test_wrapper_overlong_two_byte_nul();
    test_wrapper_overlong_three_byte();
    test_wrapper_surrogate_rejected();
    test_wrapper_above_max_codepoint();
    test_wrapper_truncated_tail();
    test_wrapper_invalid_continuation();
    test_wrapper_invalid_leading_byte();
    T_REPORT();
}
