/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "text/utf8_sanitize.h"

#define FFFD "\xEF\xBF\xBD"

static size_t sanitize_stream(const char *input, size_t input_len, char *output)
{
    struct utf8_sanitizer sanitizer;
    utf8_sanitizer_init(&sanitizer);
    size_t output_len = utf8_sanitizer_feed(&sanitizer, input, input_len, output);
    output_len += utf8_sanitizer_flush(&sanitizer, output + output_len);
    return output_len;
}

static void test_ascii_passthrough(void)
{
    char out[64];
    size_t n = sanitize_stream("hello world\n", 12, out);
    EXPECT(n == 12);
    EXPECT(memcmp(out, "hello world\n", 12) == 0);
}

static void test_valid_multibyte_passthrough(void)
{
    /* "é" (U+00E9, C3 A9), "中" (U+4E2D, E4 B8 AD), "𝓐" (U+1D4D0, F0 9D 93 90). */
    const char in[] = "\xC3\xA9\xE4\xB8\xAD\xF0\x9D\x93\x90";
    char out[32];
    size_t n = sanitize_stream(in, sizeof(in) - 1, out);
    EXPECT(n == sizeof(in) - 1);
    EXPECT(memcmp(out, in, n) == 0);
}

static void test_lone_continuation(void)
{
    char out[16];
    size_t n = sanitize_stream("\xA0", 1, out);
    EXPECT(n == 3);
    EXPECT(memcmp(out, FFFD, 3) == 0);
}

static void test_invalid_leader(void)
{
    char out[16];
    size_t n = sanitize_stream("\xFF", 1, out);
    EXPECT(n == 3);
    EXPECT(memcmp(out, FFFD, 3) == 0);
}

static void test_invalid_leader_emits_immediately(void)
{
    struct utf8_sanitizer sanitizer;
    utf8_sanitizer_init(&sanitizer);
    char output[UTF8_SANITIZE_FEED_MAX(1)];

    size_t output_len = utf8_sanitizer_feed(&sanitizer, "\xC0", 1, output);
    EXPECT(output_len == 3);
    EXPECT(memcmp(output, FFFD, 3) == 0);
    EXPECT(utf8_sanitizer_flush(&sanitizer, output) == 0);
}

static void test_flush_replaces_truncated_sequence(void)
{
    char out[16];
    size_t n = sanitize_stream("\xE4", 1, out);
    EXPECT(n == 3);
    EXPECT(memcmp(out, FFFD, 3) == 0);
}

static void test_overlong_two_byte(void)
{
    char out[16];
    size_t n = sanitize_stream("\xC0\x80", 2, out);
    EXPECT(n == 6);
    EXPECT(memcmp(out, FFFD FFFD, 6) == 0);
}

static void test_surrogate_rejected(void)
{
    /* ED A0 80 encodes the forbidden surrogate U+D800. */
    char out[16];
    size_t n = sanitize_stream("\xED\xA0\x80", 3, out);
    EXPECT(n == 9);
    EXPECT(memcmp(out, FFFD FFFD FFFD, 9) == 0);
}

static void test_above_max_codepoint(void)
{
    /* F4 90 80 80 = U+110000, one past the maximum. */
    char out[16];
    size_t n = sanitize_stream("\xF4\x90\x80\x80", 4, out);
    EXPECT(n == 12);
    EXPECT(memcmp(out, FFFD FFFD FFFD FFFD, 12) == 0);
}

static void test_nul_replaced(void)
{
    char in[] = "a\0b";
    char out[16];
    size_t n = sanitize_stream(in, 3, out);
    EXPECT(n == 5);
    EXPECT(memcmp(out, "a" FFFD "b", 5) == 0);
}

static void test_chunk_split_two_byte(void)
{
    struct utf8_sanitizer s;
    utf8_sanitizer_init(&s);
    char out[16];
    size_t n = utf8_sanitizer_feed(&s, "\xC3", 1, out);
    EXPECT(n == 0); /* leader buffered, nothing emitted yet */
    n = utf8_sanitizer_feed(&s, "\xA9", 1, out);
    EXPECT(n == 2);
    EXPECT(memcmp(out, "\xC3\xA9", 2) == 0);
    EXPECT(utf8_sanitizer_flush(&s, out) == 0);
}

static void test_chunk_split_three_byte(void)
{
    /* "中" (E4 B8 AD) split across two feeds at every boundary. */
    const char *boundaries[] = {"\xE4|\xB8\xAD", "\xE4\xB8|\xAD"};
    for (size_t i = 0; i < 2; i++) {
        struct utf8_sanitizer sanitizer;
        utf8_sanitizer_init(&sanitizer);
        const char *split = strchr(boundaries[i], '|');
        size_t first_len = (size_t)(split - boundaries[i]);
        size_t second_len = strlen(boundaries[i]) - first_len - 1;
        char output[16];
        size_t output_len = utf8_sanitizer_feed(&sanitizer, boundaries[i], first_len, output);
        output_len += utf8_sanitizer_feed(&sanitizer, split + 1, second_len, output + output_len);
        output_len += utf8_sanitizer_flush(&sanitizer, output + output_len);
        EXPECT(output_len == 3);
        EXPECT(memcmp(output, "\xE4\xB8\xAD", 3) == 0);
    }
}

static void test_invalid_continuation_aborts_sequence(void)
{
    /* Leader followed by a non-continuation byte: must replace what we
     * had and reconsider the new byte at the top. */
    struct utf8_sanitizer s;
    utf8_sanitizer_init(&s);
    char out[16];
    /* C3 ('é' leader) then 'A' (ASCII). C3 → U+FFFD, then 'A'. */
    size_t n = utf8_sanitizer_feed(&s,
                                   "\xC3"
                                   "A",
                                   2, out);
    n += utf8_sanitizer_flush(&s, out + n);
    EXPECT(n == 4);
    EXPECT(memcmp(out, FFFD "A", 4) == 0);
}

static void test_flush_idempotent(void)
{
    struct utf8_sanitizer s;
    utf8_sanitizer_init(&s);
    char out[16];
    EXPECT(utf8_sanitizer_flush(&s, out) == 0);
    size_t n = utf8_sanitizer_feed(&s, "\xE4", 1, out);
    EXPECT(n == 0);
    n = utf8_sanitizer_flush(&s, out);
    EXPECT(n == 3);
    EXPECT(utf8_sanitizer_flush(&s, out) == 0);
}

static void test_chunk_split_completes_invalid_sequence(void)
{
    /* One byte can complete a buffered invalid sequence and emit four replacements. */
    struct utf8_sanitizer s;
    utf8_sanitizer_init(&s);
    char out[UTF8_SANITIZE_FEED_MAX(8)];
    size_t n = utf8_sanitizer_feed(&s, "\xF4\x90\x80", 3, out);
    EXPECT(n == 0); /* all three bytes buffered */
    n = utf8_sanitizer_feed(&s, "\x80", 1, out);
    EXPECT(n == 12);
    EXPECT(memcmp(out, FFFD FFFD FFFD FFFD, 12) == 0);
    EXPECT(utf8_sanitizer_flush(&s, out) == 0);
}

static void test_chunk_split_aborts_with_pending_buffer(void)
{
    /* One new byte may release three buffered replacements before being reconsidered. */
    struct utf8_sanitizer s;
    utf8_sanitizer_init(&s);
    char out[UTF8_SANITIZE_FEED_MAX(8)];
    /* F4 90 80 buffers 3 bytes (4-byte sequence). */
    size_t n = utf8_sanitizer_feed(&s, "\xF4\x90\x80", 3, out);
    EXPECT(n == 0);
    /* 'A' is non-continuation: emit 3 FFFDs for buffer + 1 for 'A'. */
    n = utf8_sanitizer_feed(&s, "A", 1, out);
    EXPECT(n == 10);
    EXPECT(memcmp(out, FFFD FFFD FFFD "A", 10) == 0);
}

static void test_flush_max_three_buffered(void)
{
    /* Three buffered bytes flushed at EOF must produce exactly three
     * U+FFFDs and fit under UTF8_SANITIZE_FLUSH_MAX. */
    struct utf8_sanitizer s;
    utf8_sanitizer_init(&s);
    char out[UTF8_SANITIZE_FLUSH_MAX];
    size_t n = utf8_sanitizer_feed(&s, "\xF0\x9F\x8E", 3, out);
    EXPECT(n == 0);
    n = utf8_sanitizer_flush(&s, out);
    EXPECT(n == 9);
    EXPECT(memcmp(out, FFFD FFFD FFFD, 9) == 0);
}

static void test_byte_by_byte_equivalence(void)
{
    /* Feeding one byte at a time must produce the same output as one
     * shot, for both valid and malformed input. */
    const char in[] = "ok \xC3\xA9 \xE4\xB8\xAD trailing \xFF garbage \xC3";
    size_t in_len = sizeof(in) - 1;

    char expected[64];
    size_t expected_len = sanitize_stream(in, in_len, expected);

    struct utf8_sanitizer sanitizer;
    utf8_sanitizer_init(&sanitizer);
    char bytewise[64];
    size_t bytewise_len = 0;
    for (size_t i = 0; i < in_len; i++)
        bytewise_len += utf8_sanitizer_feed(&sanitizer, in + i, 1, bytewise + bytewise_len);
    bytewise_len += utf8_sanitizer_flush(&sanitizer, bytewise + bytewise_len);

    EXPECT(bytewise_len == expected_len);
    EXPECT(memcmp(bytewise, expected, bytewise_len) == 0);
}

static void test_one_shot_ascii(void)
{
    char *out = utf8_sanitize("hello", 5);
    EXPECT_STR_EQ(out, "hello");
    free(out);
}

static void test_one_shot_empty(void)
{
    char *out = utf8_sanitize("", 0);
    EXPECT(out != NULL);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_one_shot_nul_byte_replaced(void)
{
    char *out = utf8_sanitize("a\0b", 3);
    EXPECT_MEM_EQ(out, strlen(out), "a" FFFD "b", 5);
    free(out);
}

static void test_one_shot_valid_multibyte_preserved(void)
{
    /* "é€🎉" = C3 A9 | E2 82 AC | F0 9F 8E 89 */
    const char in[] = "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x8E\x89";
    char *out = utf8_sanitize(in, sizeof(in) - 1);
    EXPECT_STR_EQ(out, in);
    free(out);
}

static void test_one_shot_overlong_two_byte_nul(void)
{
    /* C0 80 — overlong NUL. One U+FFFD per byte. */
    char *out = utf8_sanitize("\xC0\x80", 2);
    EXPECT_STR_EQ(out, FFFD FFFD);
    free(out);
}

static void test_one_shot_overlong_three_byte(void)
{
    /* E0 80 80 — overlong encoding of U+0000 */
    char *out = utf8_sanitize("\xE0\x80\x80", 3);
    EXPECT_STR_EQ(out, FFFD FFFD FFFD);
    free(out);
}

static void test_one_shot_surrogate_rejected(void)
{
    /* ED A0 80 = U+D800, high surrogate */
    char *out = utf8_sanitize("\xED\xA0\x80", 3);
    EXPECT_STR_EQ(out, FFFD FFFD FFFD);
    free(out);
}

static void test_one_shot_above_max_codepoint(void)
{
    /* F4 90 80 80 = U+110000, beyond U+10FFFF */
    char *out = utf8_sanitize("\xF4\x90\x80\x80", 4);
    EXPECT_STR_EQ(out, FFFD FFFD FFFD FFFD);
    free(out);
}

static void test_one_shot_truncated_tail(void)
{
    char *out = utf8_sanitize("\xC2", 1);
    EXPECT_STR_EQ(out, FFFD);
    free(out);
}

static void test_one_shot_invalid_continuation(void)
{
    char *out = utf8_sanitize("\xC2 ", 2);
    EXPECT_STR_EQ(out, FFFD " ");
    free(out);
}

static void test_one_shot_invalid_leading_byte(void)
{
    char *out = utf8_sanitize("x\xFFy", 3);
    EXPECT_STR_EQ(out, "x" FFFD "y");
    free(out);
}

int main(void)
{
    test_ascii_passthrough();
    test_valid_multibyte_passthrough();
    test_lone_continuation();
    test_invalid_leader();
    test_invalid_leader_emits_immediately();
    test_flush_replaces_truncated_sequence();
    test_overlong_two_byte();
    test_surrogate_rejected();
    test_above_max_codepoint();
    test_nul_replaced();
    test_chunk_split_two_byte();
    test_chunk_split_three_byte();
    test_invalid_continuation_aborts_sequence();
    test_chunk_split_completes_invalid_sequence();
    test_chunk_split_aborts_with_pending_buffer();
    test_flush_max_three_buffered();
    test_flush_idempotent();
    test_byte_by_byte_equivalence();
    test_one_shot_ascii();
    test_one_shot_empty();
    test_one_shot_nul_byte_replaced();
    test_one_shot_valid_multibyte_preserved();
    test_one_shot_overlong_two_byte_nul();
    test_one_shot_overlong_three_byte();
    test_one_shot_surrogate_rejected();
    test_one_shot_above_max_codepoint();
    test_one_shot_truncated_tail();
    test_one_shot_invalid_continuation();
    test_one_shot_invalid_leading_byte();
    T_REPORT();
}
