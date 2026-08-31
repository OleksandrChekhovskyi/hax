/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "system/locale.h"
#include "terminal/ansi.h"
#include "terminal/vt_resolve.h"

/* Resolve `in` and return the settled rows. Caller frees. */
static char *resolve_rows(const char *in)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    if (!mem) {
        perror("open_memstream");
        exit(1);
    }
    vt_resolve(in, strlen(in), mem);
    fclose(mem);
    return buf;
}

static void test_plain_rows_pass_through(void)
{
    char *out = resolve_rows("hello\nworld\n");
    EXPECT_STR_EQ(out, "hello\nworld\n");
    free(out);
}

static void test_partial_row_terminated(void)
{
    char *out = resolve_rows("tail");
    EXPECT_STR_EQ(out, "tail\n");
    free(out);
}

static void test_retro_wrap_erases_partial_word(void)
{
    char *out = resolve_rows("alpha beta"
                             "\x1b[5D" ANSI_ERASE_LINE "\nbeta\n");
    EXPECT_STR_EQ(out, "alpha\nbeta\n");
    free(out);
}

static void test_erase_keeps_style_runs(void)
{
    char *out = resolve_rows("ab" ANSI_BOLD "cd"
                             "\x1b[2D" ANSI_ERASE_LINE "\n");
    EXPECT_STR_EQ(out, "ab" ANSI_BOLD ANSI_RESET "\n");
    free(out);
}

static void test_carriage_return_overprints_first_cell(void)
{
    char *out = resolve_rows("| body\r+\n");
    EXPECT_STR_EQ(out, "+ body\n");
    free(out);
}

static void test_overprint_keeps_surrounding_style(void)
{
    char *out = resolve_rows(ANSI_DIM "|" ANSI_RESET " body\r" ANSI_BOLD "+" ANSI_RESET "\n");
    EXPECT_STR_EQ(out, ANSI_DIM ANSI_BOLD "+" ANSI_RESET ANSI_RESET " body\n");
    free(out);
}

static void test_user_echo_row_break(void)
{
    char *out = resolve_rows("| one" ANSI_ERASE_LINE "\r\n| two" ANSI_ERASE_LINE "\r\n");
    EXPECT_STR_EQ(out, "| one\n| two\n");
    free(out);
}

static void test_newline_keeps_rest_of_row(void)
{
    char *out = resolve_rows("abcdef\rXY\n");
    EXPECT_STR_EQ(out, "XYcdef\n");
    free(out);
}

static void test_multibyte_glyph_columns(void)
{
    char *out = resolve_rows("\xE2\x94\x82\xE2\x94\x82"
                             "\x1b[1D" ANSI_ERASE_LINE "\n");
    EXPECT_STR_EQ(out, "\xE2\x94\x82\n");
    free(out);
}

static void test_cursor_forward_pads(void)
{
    char *out = resolve_rows("ab"
                             "\x1b[3C"
                             "c\n");
    EXPECT_STR_EQ(out, "ab   c\n");
    free(out);
}

/* Assistant output can contain arbitrary cursor positions; resolving it must stay bounded. */
static void test_absurd_cursor_forward_is_clamped(void)
{
    char *out = resolve_rows("a\x1b[2147483647C"
                             "b\n");
    EXPECT(strlen(out) < 8192);
    EXPECT(out[0] == 'a');
    EXPECT(strchr(out, 'b') != NULL);
    free(out);

    /* Same for a digit run too long for the accumulator. */
    char *digits = resolve_rows("a\x1b[99999999999999999999C"
                                "b\n");
    EXPECT(strlen(digits) < 8192);
    EXPECT(strchr(digits, 'b') != NULL);
    free(digits);
}

static void test_unmodeled_escape_passes_through(void)
{
    char *out = resolve_rows("a\x1b[2Ab\n");
    EXPECT_STR_EQ(out, "a\x1b[2Ab\n");
    free(out);
}

static void test_double_width_glyph_columns(void)
{
    char *out = resolve_rows("ab\xE4\xBD\xA0"
                             "\x1b[2D" ANSI_ERASE_LINE "\n");
    EXPECT_STR_EQ(out, "ab\n");
    free(out);

    char *half = resolve_rows("ab\xE4\xBD\xA0"
                              "\x1b[1D" ANSI_ERASE_LINE "\n");
    EXPECT_STR_EQ(half, "ab\n");
    free(half);
}

static void test_combining_mark_is_zero_width(void)
{
    /* "e" + U+0301 combining acute, then back one column and erase. */
    char *out = resolve_rows("xe\xCC\x81"
                             "\x1b[1D" ANSI_ERASE_LINE "\n");
    EXPECT_STR_EQ(out, "x\n");
    free(out);
}

static void test_erase_keeps_combining_mark_with_surviving_base(void)
{
    char *out = resolve_rows("e\xCC\x81x\x1b[1D" ANSI_ERASE_LINE "\n");
    EXPECT_STR_EQ(out, "e\xCC\x81\n");
    free(out);
}

static void test_erase_removes_combining_mark_without_base(void)
{
    char *out = resolve_rows("\xCC\x81"
                             "a\r" ANSI_ERASE_LINE "\n");
    EXPECT_STR_EQ(out, "\n");
    free(out);
}

static void test_zero_width_writes_keep_order_at_moved_cursor(void)
{
    char *out = resolve_rows("a\r\xCC\x81\xCC\xA7" ANSI_RED "\n");
    EXPECT_STR_EQ(out, "\xCC\x81\xCC\xA7" ANSI_RED "a" ANSI_RESET "\n");
    free(out);
}

static void test_cursor_back_clamps_at_column_zero(void)
{
    char *out = resolve_rows("abc\x1b[99D"
                             "Z\n");
    EXPECT_STR_EQ(out, "Zbc\n");
    free(out);
}

static void test_unterminated_escape_consumed(void)
{
    char *out = resolve_rows("ab\x1b[");
    EXPECT_STR_EQ(out, "ab\x1b[\n");
    free(out);

    char *bare = resolve_rows("ab\x1b");
    EXPECT_STR_EQ(bare, "ab\x1b\n");
    free(bare);
}

static void test_osc_passes_through_whole(void)
{
    char *out = resolve_rows("a\x1b]8;;http://x\x07"
                             "b\n");
    EXPECT_STR_EQ(out, "a\x1b]8;;http://x\x07"
                       "b\n");
    free(out);
}

static void test_multi_parameter_sgr_passes_through(void)
{
    char *out = resolve_rows("\x1b[38;5;173mtinted\x1b[39m\n");
    EXPECT_STR_EQ(out, "\x1b[38;5;173mtinted\x1b[39m\n");
    free(out);
}

/* Reasoning blocks open dim italic once for many rows; a pager resets SGR state at every line,
 * so each settled row must reopen the carried styling itself. */
static void test_styled_run_reopens_on_each_row(void)
{
    char *out = resolve_rows(ANSI_DIM ANSI_ITALIC "one\ntwo\nthree" ANSI_RESET "\nplain\n");
    EXPECT_STR_EQ(out, ANSI_DIM ANSI_ITALIC "one" ANSI_RESET "\n"
                                            "\x1b[2;3mtwo" ANSI_RESET "\n"
                                            "\x1b[2;3mthree" ANSI_RESET "\nplain\n");
    free(out);
}

static void test_extended_color_reopens_verbatim(void)
{
    char *out = resolve_rows("\x1b[38;5;173mone\ntwo\x1b[39m\n");
    EXPECT_STR_EQ(out, "\x1b[38;5;173mone" ANSI_RESET "\n"
                       "\x1b[38;5;173mtwo\x1b[39m\n");
    free(out);
}

static void test_blank_row_inside_styled_run_stays_bare(void)
{
    char *out = resolve_rows(ANSI_BOLD "one\n\ntwo" ANSI_RESET "\n");
    EXPECT_STR_EQ(out, ANSI_BOLD "one" ANSI_RESET "\n\n" ANSI_BOLD "two" ANSI_RESET "\n");
    free(out);
}

static void test_selective_sgr_off_codes_stop_the_carry(void)
{
    char *out = resolve_rows(ANSI_BOLD ANSI_ITALIC "a" ANSI_BOLD_OFF "b\nc\n");
    EXPECT_STR_EQ(out, ANSI_BOLD ANSI_ITALIC "a" ANSI_BOLD_OFF "b" ANSI_RESET "\n" ANSI_ITALIC
                                             "c" ANSI_RESET "\n");
    free(out);
}

/* xterm modifyOtherKeys also ends in 'm'; it must pass through without touching the carry. */
static void test_private_final_m_sequence_is_not_tracked(void)
{
    char *out = resolve_rows(ANSI_BOLD "a\x1b[>4;2mb\nc" ANSI_RESET "\n");
    EXPECT_STR_EQ(out, ANSI_BOLD "a\x1b[>4;2mb" ANSI_RESET "\n" ANSI_BOLD "c" ANSI_RESET "\n");
    free(out);
}

static void test_only_first_csi_parameter_controls_erase(void)
{
    char *out = resolve_rows("abcdef\x1b[4D\x1b[1;99KZ\n");
    EXPECT_STR_EQ(out, "  Zdef\n");
    free(out);
}

static void test_empty_input(void)
{
    char *out = resolve_rows("");
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_erase_whole_row(void)
{
    char *out = resolve_rows("junk\x1b[2Kkept\n");
    EXPECT_STR_EQ(out, "    kept\n");
    free(out);
}

static void test_erase_to_cursor_keeps_right_side(void)
{
    char *out = resolve_rows("abcdef"
                             "\x1b[4D\x1b[1K"
                             "Z\n");
    EXPECT_STR_EQ(out, "  Zdef\n");
    free(out);

    char *tail = resolve_rows("abc\x1b[1KZ\n");
    EXPECT_STR_EQ(tail, "   Z\n");
    free(tail);
}

int main(void)
{
    locale_init_utf8();
    test_plain_rows_pass_through();
    test_partial_row_terminated();
    test_retro_wrap_erases_partial_word();
    test_erase_keeps_style_runs();
    test_carriage_return_overprints_first_cell();
    test_overprint_keeps_surrounding_style();
    test_user_echo_row_break();
    test_newline_keeps_rest_of_row();
    test_multibyte_glyph_columns();
    test_cursor_forward_pads();
    test_absurd_cursor_forward_is_clamped();
    test_unmodeled_escape_passes_through();
    test_double_width_glyph_columns();
    test_combining_mark_is_zero_width();
    test_erase_keeps_combining_mark_with_surviving_base();
    test_erase_removes_combining_mark_without_base();
    test_zero_width_writes_keep_order_at_moved_cursor();
    test_cursor_back_clamps_at_column_zero();
    test_unterminated_escape_consumed();
    test_osc_passes_through_whole();
    test_multi_parameter_sgr_passes_through();
    test_styled_run_reopens_on_each_row();
    test_extended_color_reopens_verbatim();
    test_blank_row_inside_styled_run_stays_bare();
    test_selective_sgr_off_codes_stop_the_carry();
    test_private_final_m_sequence_is_not_tracked();
    test_only_first_csi_parameter_controls_erase();
    test_empty_input();
    test_erase_whole_row();
    test_erase_to_cursor_keeps_right_side();
    T_REPORT();
}
