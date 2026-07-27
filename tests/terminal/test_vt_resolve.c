/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "util.h"
#include "terminal/vt_resolve.h"
#include "terminal/ansi.h"

/* Resolve `in` and return the settled rows. Caller frees. */
static char *resolve(const char *in)
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
    char *out = resolve("hello\nworld\n");
    EXPECT_STR_EQ(out, "hello\nworld\n");
    free(out);
}

/* A trailing row with no \n is still content the terminal would be
 * showing, so it is terminated rather than dropped. */
static void test_partial_row_terminated(void)
{
    char *out = resolve("tail");
    EXPECT_STR_EQ(out, "tail\n");
    free(out);
}

/* The markdown wrapper's retro-wrap shape: emit a partial word, walk
 * back over it, erase to end of row, break, replay it on the next row. */
static void test_retro_wrap_erases_partial_word(void)
{
    char *out = resolve("alpha beta"
                        "\x1b[5D" ANSI_ERASE_LINE "\nbeta\n");
    EXPECT_STR_EQ(out, "alpha\nbeta\n");
    free(out);
}

/* Erase-to-end drops the columns from the cursor on, but keeps the
 * zero-width styling runs among them: on a real terminal an erase
 * clears cells without resetting pending SGR state. */
static void test_erase_keeps_style_runs(void)
{
    char *out = resolve("ab" ANSI_BOLD "cd"
                        "\x1b[2D" ANSI_ERASE_LINE "\n");
    EXPECT_STR_EQ(out, "ab" ANSI_BOLD "\n");
    free(out);
}

/* disp_tool_strip_close's shape: overprint the row's leading glyph with
 * the closing one, leaving the rest of the row untouched. */
static void test_carriage_return_overprints_first_cell(void)
{
    char *out = resolve("| body\r+\n");
    EXPECT_STR_EQ(out, "+ body\n");
    free(out);
}

/* The overprint keeps its own styling and does not disturb the styling
 * of the content that follows on the row. */
static void test_overprint_keeps_surrounding_style(void)
{
    char *out = resolve(ANSI_DIM "|" ANSI_RESET " body\r" ANSI_BOLD "+" ANSI_RESET "\n");
    EXPECT_STR_EQ(out, ANSI_DIM ANSI_BOLD "+" ANSI_RESET ANSI_RESET " body\n");
    free(out);
}

/* The user-echo row terminator (erase-line + \r\n) settles to a plain
 * row break, and the stripe prefix repeats per row as it does live. */
static void test_user_echo_row_break(void)
{
    char *out = resolve("| one" ANSI_ERASE_LINE "\r\n| two" ANSI_ERASE_LINE "\r\n");
    EXPECT_STR_EQ(out, "| one\n| two\n");
    free(out);
}

/* A \n from mid-row keeps the columns to the right of the cursor: the
 * cursor moved, the cells were never cleared. */
static void test_newline_keeps_rest_of_row(void)
{
    char *out = resolve("abcdef\rXY\n");
    EXPECT_STR_EQ(out, "XYcdef\n");
    free(out);
}

/* Glyphs are addressed by cell, not byte: walking back one column
 * lands on a whole multi-byte codepoint, never half of one. */
static void test_multibyte_glyph_columns(void)
{
    char *out = resolve("\xE2\x94\x82\xE2\x94\x82"
                        "\x1b[1D" ANSI_ERASE_LINE "\n");
    EXPECT_STR_EQ(out, "\xE2\x94\x82\n");
    free(out);
}

/* Cursor-forward past the end pads with spaces rather than dropping
 * the motion. */
static void test_cursor_forward_pads(void)
{
    char *out = resolve("ab"
                        "\x1b[3C"
                        "c\n");
    EXPECT_STR_EQ(out, "ab   c\n");
    free(out);
}

/* Nothing strips escapes out of assistant text, so the column in a
 * cursor-forward can be anything a model cares to type — and every cell it
 * parks past the end of the row becomes padding when the next glyph lands.
 * Clamped to VT_MAX_COL, this stays a row; unclamped it allocates ~2GB of
 * spaces (or overflows the parse first, which is undefined behavior). */
static void test_absurd_cursor_forward_is_clamped(void)
{
    char *out = resolve("a\x1b[2147483647C"
                        "b\n");
    EXPECT(strlen(out) < 8192);
    EXPECT(out[0] == 'a');
    EXPECT(strchr(out, 'b') != NULL);
    free(out);

    /* Same for a digit run too long for the accumulator. */
    char *digits = resolve("a\x1b[99999999999999999999C"
                           "b\n");
    EXPECT(strlen(digits) < 8192);
    EXPECT(strchr(digits, 'b') != NULL);
    free(digits);
}

/* Vertical motion is deliberately unmodeled — it must pass through
 * inert instead of corrupting the row model. */
static void test_unmodeled_escape_passes_through(void)
{
    char *out = resolve("a\x1b[2Ab\n");
    EXPECT_STR_EQ(out, "a\x1b[2Ab\n");
    free(out);
}

/* A double-width glyph occupies two columns, so a cursor-back of 2 lands
 * before it and erases it whole — cell arithmetic, not byte arithmetic.
 * (U+4F60 你 is wide; the ASCII tail proves the split point.) */
static void test_double_width_glyph_columns(void)
{
    char *out = resolve("ab\xE4\xBD\xA0"
                        "\x1b[2D" ANSI_ERASE_LINE "\n");
    EXPECT_STR_EQ(out, "ab\n");
    free(out);

    /* Backing up only one column lands mid-glyph; the erase must not cut
     * the codepoint in half — it takes the whole cell it starts inside. */
    char *half = resolve("ab\xE4\xBD\xA0"
                         "\x1b[1D" ANSI_ERASE_LINE "\n");
    EXPECT_STR_EQ(half, "ab\n");
    free(half);
}

/* A combining mark is zero-width: it rides the glyph before it and doesn't
 * shift the column the cursor ops count against. */
static void test_combining_mark_is_zero_width(void)
{
    /* "e" + U+0301 combining acute, then back one column and erase. */
    char *out = resolve("xe\xCC\x81"
                        "\x1b[1D" ANSI_ERASE_LINE "\n");
    EXPECT_STR_EQ(out, "x\n");
    free(out);
}

/* Backing up past column 0 clamps instead of wrapping around into a huge
 * unsigned column. */
static void test_cursor_back_clamps_at_column_zero(void)
{
    char *out = resolve("abc\x1b[99D"
                        "Z\n");
    EXPECT_STR_EQ(out, "Zbc\n");
    free(out);
}

/* Truncated escapes at end of input must be consumed, not re-scanned as
 * content — a stray CSI leader can arrive when a render is cut short. */
static void test_unterminated_escape_consumed(void)
{
    char *out = resolve("ab\x1b[");
    EXPECT_STR_EQ(out, "ab\x1b[\n");
    free(out);

    char *bare = resolve("ab\x1b");
    EXPECT_STR_EQ(bare, "ab\x1b\n");
    free(bare);
}

/* An OSC string (title set, hyperlink) is zero-width and passes through
 * whole — its embedded ';' and text must not be read as CSI parameters. */
static void test_osc_passes_through_whole(void)
{
    char *out = resolve("a\x1b]8;;http://x\x07"
                        "b\n");
    EXPECT_STR_EQ(out, "a\x1b]8;;http://x\x07"
                       "b\n");
    free(out);
}

/* Only the first CSI parameter is consulted, and a multi-parameter SGR run
 * (the theme's 38;5;N colors) stays intact as zero-width bytes. */
static void test_multi_parameter_sgr_intact(void)
{
    char *out = resolve("\x1b[38;5;173mtinted\x1b[39m\n");
    EXPECT_STR_EQ(out, "\x1b[38;5;173mtinted\x1b[39m\n");
    free(out);
}

/* Empty input produces nothing — no stray newline from the trailing-row
 * flush. */
static void test_empty_input(void)
{
    char *out = resolve("");
    EXPECT_STR_EQ(out, "");
    free(out);
}

/* CSI 2K clears the whole row but does *not* move the cursor, so what
 * follows lands at the column it was already at — over blanked cells. (Only
 * 0K comes from hax itself; the other forms arrive in model text, so they
 * have to settle the way a terminal would rather than conveniently.) */
static void test_erase_whole_row(void)
{
    char *out = resolve("junk\x1b[2Kkept\n");
    EXPECT_STR_EQ(out, "    kept\n");
    free(out);
}

/* CSI 1K clears from the row start through the cursor's own cell and leaves
 * the columns to its right where they were — so the blanked span stays as
 * spaces holding them in place. */
static void test_erase_to_cursor_keeps_right_side(void)
{
    /* "abcdef", back to column 2, erase columns 0-2, write at column 2. */
    char *out = resolve("abcdef"
                        "\x1b[4D\x1b[1K"
                        "Z\n");
    EXPECT_STR_EQ(out, "  Zdef\n");
    free(out);

    /* With nothing to the right, the cleared cells need no placeholder. */
    char *tail = resolve("abc\x1b[1KZ\n");
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
    test_cursor_back_clamps_at_column_zero();
    test_unterminated_escape_consumed();
    test_osc_passes_through_whole();
    test_multi_parameter_sgr_intact();
    test_empty_input();
    test_erase_whole_row();
    test_erase_to_cursor_keeps_right_side();
    T_REPORT();
}
