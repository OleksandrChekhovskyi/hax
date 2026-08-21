/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "render/ctrl_strip.h"

static char *strip_bytes(const char *input, size_t input_len, size_t *output_len)
{
    char *output = malloc(input_len + 1);
    struct ctrl_strip strip;

    ctrl_strip_init(&strip);
    *output_len = ctrl_strip_feed(&strip, input, input_len, output);
    output[*output_len] = '\0';
    return output;
}

static char *strip_single_chunk(const char *input)
{
    size_t output_len;

    return strip_bytes(input, strlen(input), &output_len);
}

static char *strip_one_byte_chunks(const char *input)
{
    size_t input_len = strlen(input);
    char *output = malloc(input_len + 1);
    struct ctrl_strip strip;
    size_t output_len = 0;

    ctrl_strip_init(&strip);
    for (size_t i = 0; i < input_len; i++)
        output_len += ctrl_strip_feed(&strip, input + i, 1, output + output_len);
    output[output_len] = '\0';
    return output;
}

static void test_passthrough(void)
{
    char *got = strip_single_chunk("hello world\n\ttabbed\n");
    EXPECT_STR_EQ(got, "hello world\n\ttabbed\n");
    free(got);
}

static void test_csi_sgr(void)
{
    char *got = strip_single_chunk("\x1b[31mred\x1b[0m and \x1b[1;33mbold-yellow\x1b[m");
    EXPECT_STR_EQ(got, "red and bold-yellow");
    free(got);
}

static void test_csi_cursor(void)
{
    char *got = strip_single_chunk("\x1b[2J\x1b[H\x1b[10;20Hhi");
    EXPECT_STR_EQ(got, "hi");
    free(got);
}

static void test_osc_bel(void)
{
    char *got = strip_single_chunk("before\x1b]0;window title\x07"
                                   "after");
    EXPECT_STR_EQ(got, "beforeafter");
    free(got);
}

static void test_osc_st(void)
{
    char *got = strip_single_chunk("a\x1b]8;;https://example.com\x1b\\link\x1b]8;;\x1b\\b");
    EXPECT_STR_EQ(got, "alinkb");
    free(got);
}

static void test_st_terminated_control_strings(void)
{
    char *got = strip_single_chunk("a\x1bPdcs\x1b\\b\x1b^pm\x1b\\c\x1b_apc\x1b\\d");
    EXPECT_STR_EQ(got, "abcd");
    free(got);
}

static void test_single_byte_esc(void)
{
    char *got = strip_single_chunk("a\x1b"
                                   "cb\x1b=c\x1b>d");
    EXPECT_STR_EQ(got, "abcd");
    free(got);
}

static void test_intermediate_esc(void)
{
    char *got = strip_single_chunk("a\x1b(Bb\x1b)0c");
    EXPECT_STR_EQ(got, "abc");
    free(got);
}

static void test_bare_cr_dropped(void)
{
    char *got = strip_single_chunk("loading...\rdone\n");
    EXPECT_STR_EQ(got, "loading...done\n");
    free(got);
}

static void test_crlf_preserved_as_lf(void)
{
    char *got = strip_single_chunk("line1\r\nline2\r\n");
    EXPECT_STR_EQ(got, "line1\nline2\n");
    free(got);
}

static void test_backspace_and_bell_dropped(void)
{
    char *got = strip_single_chunk("a\bb\ac");
    EXPECT_STR_EQ(got, "abc");
    free(got);
}

static void test_form_feed_and_vt_dropped(void)
{
    char *got = strip_single_chunk("a\fb\vc");
    EXPECT_STR_EQ(got, "abc");
    free(got);
}

static void test_so_si_dropped(void)
{
    char *got = strip_single_chunk("a\x0e"
                                   "b\x0f"
                                   "c");
    EXPECT_STR_EQ(got, "abc");
    free(got);
}

static void test_misc_c0_dropped(void)
{
    char *got = strip_single_chunk("a\x01"
                                   "b\x06"
                                   "c\x10"
                                   "d\x18"
                                   "e\x1a"
                                   "f\x1c"
                                   "g\x1f"
                                   "h");
    EXPECT_STR_EQ(got, "abcdefgh");
    free(got);
}

static void test_del_dropped(void)
{
    char *got = strip_single_chunk("a\x7f"
                                   "b");
    EXPECT_STR_EQ(got, "ab");
    free(got);
}

static void test_embedded_nul(void)
{
    const char input[] = {'a', 0x00, 'b', 0x00, 'c'};
    size_t output_len = 0;
    char *got = strip_bytes(input, sizeof(input), &output_len);
    EXPECT_MEM_EQ(got, output_len, "abc", 3);
    free(got);
}

static void test_high_bytes_preserved(void)
{
    char *got = strip_single_chunk("\x1b[32m\xe2\x9c\x93\x1b[0m ok");
    EXPECT_STR_EQ(got, "\xe2\x9c\x93 ok");
    free(got);
}

static void test_one_byte_chunks_match_single_chunk(void)
{
    static const struct {
        const char *input;
        const char *expected;
    } cases[] = {
        {"pre\x1b[31mRED\x1b[0m\x1b]0;t\x07mid\x1b(B\fend\n", "preREDmidend\n"},
        {"a\x1b]8;;u\x1b\\b", "ab"},
        {"a\x1bP1;2;3qstuff\x1b\\b", "ab"},
        {"a\x1b(Bb\x1b)0c", "abc"},
        {"a\x1b[\nb", "a\nb"},
        {"a\x1b]title\x18"
         "b",
         "ab"},
        {"a\x1bPdcs\x1a"
         "b",
         "ab"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *single_chunk = strip_single_chunk(cases[i].input);
        char *one_byte_chunks = strip_one_byte_chunks(cases[i].input);

        EXPECT_STR_EQ(single_chunk, one_byte_chunks);
        EXPECT_STR_EQ(single_chunk, cases[i].expected);
        free(single_chunk);
        free(one_byte_chunks);
    }
}

static void test_malformed_escape_across_chunks(void)
{
    char output[8];
    struct ctrl_strip strip;
    size_t output_len = 0;

    ctrl_strip_init(&strip);
    output_len += ctrl_strip_feed(&strip, "\x1b", 1, output + output_len);
    output_len += ctrl_strip_feed(&strip, "\nX", 2, output + output_len);
    output[output_len] = '\0';
    EXPECT_STR_EQ(output, "\nX");
}

static void test_lf_cancels_csi(void)
{
    char *got = strip_single_chunk("\x1b[\n\n\nimportant message\n");
    EXPECT_STR_EQ(got, "\n\n\nimportant message\n");
    free(got);
}

static void test_can_cancels_osc(void)
{
    char *got = strip_single_chunk("\x1b]2;title\x18rest\n");
    EXPECT_STR_EQ(got, "rest\n");
    free(got);
}

static void test_sub_cancels_dcs(void)
{
    char *got = strip_single_chunk("\x1bP1;2;3qpayload\x1arest\n");
    EXPECT_STR_EQ(got, "rest\n");
    free(got);
}

static void test_lf_cancels_control_string_after_escape(void)
{
    char *osc = strip_single_chunk("a\x1b]title\x1b\nb");
    char *dcs = strip_single_chunk("a\x1bPpayload\x1b\nb");

    EXPECT_STR_EQ(osc, "a\nb");
    EXPECT_STR_EQ(dcs, "a\nb");
    free(osc);
    free(dcs);
}

static void test_incomplete_escape_removed(void)
{
    char *csi = strip_single_chunk("ok\x1b[3");
    char *osc = strip_single_chunk("ok\x1b]0;tit");
    char *escape = strip_single_chunk("ok\x1b");

    EXPECT_STR_EQ(csi, "ok");
    EXPECT_STR_EQ(osc, "ok");
    EXPECT_STR_EQ(escape, "ok");
    free(csi);
    free(osc);
    free(escape);
}

static void test_dup_helper(void)
{
    char *got = ctrl_strip_dup("\x1b[1mhi\x1b[0m\f\n");
    EXPECT_STR_EQ(got, "hi\n");
    free(got);
}

static void test_line_dup_helper(void)
{
    char *got = ctrl_strip_line_dup("\x1b[31mlimited\x1b[0m\nfake\terror");
    EXPECT_STR_EQ(got, "limited fake error");
    free(got);
}

int main(void)
{
    test_passthrough();
    test_csi_sgr();
    test_csi_cursor();
    test_osc_bel();
    test_osc_st();
    test_st_terminated_control_strings();
    test_single_byte_esc();
    test_intermediate_esc();
    test_bare_cr_dropped();
    test_crlf_preserved_as_lf();
    test_backspace_and_bell_dropped();
    test_form_feed_and_vt_dropped();
    test_so_si_dropped();
    test_misc_c0_dropped();
    test_del_dropped();
    test_embedded_nul();
    test_high_bytes_preserved();
    test_one_byte_chunks_match_single_chunk();
    test_malformed_escape_across_chunks();
    test_lf_cancels_csi();
    test_can_cancels_osc();
    test_sub_cancels_dcs();
    test_lf_cancels_control_string_after_escape();
    test_incomplete_escape_removed();
    test_dup_helper();
    test_line_dup_helper();
    T_REPORT();
}
