/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "system/locale.h"
#include "text/width.h"

/* ---------- flatten_for_display ---------- */

static void test_flatten_null(void)
{
    char *out = flatten_for_display(NULL);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_flatten_empty(void)
{
    char *out = flatten_for_display("");
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_flatten_plain(void)
{
    char *out = flatten_for_display("ls -la");
    EXPECT_STR_EQ(out, "ls -la");
    free(out);
}

static void test_flatten_newline(void)
{
    char *out = flatten_for_display("ls\npwd");
    EXPECT_STR_EQ(out, "ls pwd");
    free(out);
}

static void test_flatten_collapses_runs(void)
{
    /* Multiple newlines/tabs/spaces collapse to a single space. */
    char *out = flatten_for_display("a\n\n\tb  \r\n c");
    EXPECT_STR_EQ(out, "a b c");
    free(out);
}

static void test_flatten_strips_edges(void)
{
    char *out = flatten_for_display("\n  hello world\n\n");
    EXPECT_STR_EQ(out, "hello world");
    free(out);
}

static void test_flatten_all_whitespace(void)
{
    /* All-whitespace input collapses to empty — leading-trim drops the
     * first run, trailing-trim drops everything that came after. */
    char *out = flatten_for_display("  \n\t\r  ");
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_flatten_control_bytes(void)
{
    /* All ASCII control bytes (incl. DEL 0x7f) collapse to spaces. */
    char *out = flatten_for_display("a\x01\x02\x03"
                                    "b\x7f"
                                    "c");
    EXPECT_STR_EQ(out, "a b c");
    free(out);
}

static void test_display_cells(void)
{
    /* ASCII: cells == bytes. */
    EXPECT(display_cells("abc") == 3);
    EXPECT(display_cells("") == 0);
    EXPECT(display_cells(NULL) == 0);
    /* Multi-byte codepoint occupying one cell: bytes over-count. */
    EXPECT(display_cells("café") == 4);
    /* Wide CJK codepoint: two cells. */
    EXPECT(display_cells("a\xE4\xB8\xAD") == 3);
    /* Combining mark rides on the base glyph (zero cells). */
    EXPECT(display_cells("e\xCC\x81") == 1);
}

static void test_flatten_preserves_high_bytes(void)
{
    /* Printable UTF-8 passes through. */
    char *out = flatten_for_display("café\nlatte");
    EXPECT_STR_EQ(out, "café latte");
    free(out);
}

static void test_flatten_substitutes_bidi_override(void)
{
    /* Trojan Source: U+202E RIGHT-TO-LEFT OVERRIDE encoded as
     * E2 80 AE. Flatten substitutes with '?' so a model-supplied
     * tool arg can't bidi-reorder the rendered header. */
    char *out = flatten_for_display("ab\xE2\x80\xAE"
                                    "cd");
    EXPECT_STR_EQ(out, "ab?cd");
    free(out);
}

static void test_flatten_substitutes_zwj(void)
{
    /* U+200D ZERO WIDTH JOINER (E2 80 8D). Width-zero invisible —
     * substituted so the displayed string matches the cell budget. */
    char *out = flatten_for_display("ab\xE2\x80\x8D"
                                    "cd");
    EXPECT_STR_EQ(out, "ab?cd");
    free(out);
}

static void test_flatten_substitutes_malformed_utf8(void)
{
    /* Lone continuation byte: malformed UTF-8 → '?'. */
    char *out = flatten_for_display("ab\x80"
                                    "cd");
    EXPECT_STR_EQ(out, "ab?cd");
    free(out);
}

static void test_flatten_caps_zero_width_run(void)
{
    /* Bound bytes consumed by a visually zero-width run. */
    char input[1 + 2 * 100 + 1];
    input[0] = 'a';
    for (int k = 0; k < 100; k++) {
        input[1 + 2 * k] = (char)0xCC;
        input[2 + 2 * k] = (char)0x81;
    }
    input[1 + 2 * 100] = '\0';
    char *out = flatten_for_display(input);
    /* "a" + 8 combining marks = 1 + 16 = 17 bytes. */
    EXPECT(strlen(out) == 17);
    EXPECT(out[0] == 'a');
    free(out);
}

static void test_flatten_preserves_legit_combining_run(void)
{
    /* Below the cap, combining marks pass through unchanged so
     * legitimate decomposed forms (e.g. macOS HFS+ NFD paths,
     * Devanagari with multiple marks per base) render correctly.
     * "a" + 3 combining marks = 1 + 6 = 7 bytes, unchanged. */
    char *out = flatten_for_display("a\xCC\x81\xCC\x81\xCC\x81");
    EXPECT_STR_EQ(out, "a\xCC\x81\xCC\x81\xCC\x81");
    free(out);
}

/* ---------- truncate_for_display ---------- */

static void test_truncate_under_cap(void)
{
    /* String shorter than cap: returned unchanged (still a fresh dup). */
    char *out = truncate_for_display("hello", 10);
    EXPECT_STR_EQ(out, "hello");
    free(out);
}

static void test_truncate_exact_cap(void)
{
    /* strlen == cap: still a no-op. */
    char *out = truncate_for_display("hello", 5);
    EXPECT_STR_EQ(out, "hello");
    free(out);
}

static void test_truncate_above_cap(void)
{
    /* strlen > cap: cut to cap-3 bytes and append "...". */
    char *out = truncate_for_display("hello world", 8);
    EXPECT_STR_EQ(out, "hello...");
    free(out);
}

static void test_truncate_tiny_cap(void)
{
    /* cap < 4 has no room for "..." — hard cut, no marker. */
    char *out = truncate_for_display("hello", 3);
    EXPECT_STR_EQ(out, "hel");
    free(out);
}

static void test_truncate_utf8_boundary(void)
{
    /* "café latte" — 10 cells (each codepoint is 1 cell here),
     * 11 bytes (é is 2 bytes). Cap of 5 cells: cut at 2 cells
     * (cap-3) and append "...". Result "ca...". The é is preserved
     * as a unit (no half-codepoint). */
    char *out = truncate_for_display("café latte", 5);
    EXPECT_STR_EQ(out, "ca...");
    free(out);
}

static void test_truncate_keeps_multibyte_intact(void)
{
    /* Cap of 6 cells over "café latte" (10 cells): keeps "caf"
     * (3 cells) plus "..." since cap-3=3. The é codepoint is past
     * the cut and excluded entirely — never split mid-byte. */
    char *out = truncate_for_display("café latte", 6);
    EXPECT_STR_EQ(out, "caf...");
    free(out);
}

static void test_truncate_under_cap_multibyte(void)
{
    /* "café" = 4 cells (each codepoint is 1 cell, é via wcwidth=1)
     * but 5 bytes. cap=4 fits even though byte length exceeds cap —
     * cells, not bytes, drive the decision. */
    char *out = truncate_for_display("café", 4);
    EXPECT_STR_EQ(out, "café");
    free(out);
}

static void test_truncate_keeps_trailing_combining_mark(void)
{
    /* "abcd" + COMBINING ACUTE on d (U+0301) = 4 cells, 6 bytes.
     * cap=4 should fit it whole — combining marks contribute 0 cells
     * and ride on the prior glyph. Without absorbing trailing zero-
     * width codepoints, advance_cells stops after 'd' and the cut
     * orphans the combining mark, mis-rendering as "a..." or similar. */
    char *out = truncate_for_display("abcd\xCC\x81", 4);
    EXPECT_STR_EQ(out, "abcd\xCC\x81");
    free(out);
}

static void test_truncate_emoji_two_cells(void)
{
    /* Wide codepoints take 2 cells. "🦀abc" = 1 emoji (2 cells) +
     * 3 ASCII = 5 cells, 7 bytes. cap=4 means the emoji + 1 char fits
     * with room for nothing else; we cut at 1 cell (emoji is 2 → can't
     * include) and append "...". cap-3=1 cell budget → cut after the
     * first ASCII codepoint that fits, i.e. nothing fits, hard cut at
     * byte 0 + "...". Result "...". */
    char *out = truncate_for_display("\xF0\x9F\xA6\x80"
                                     "abc",
                                     4);
    EXPECT_STR_EQ(out, "...");
    free(out);

    /* cap=5 fits the full string (2+1+1+1 = 5 cells). */
    char *out2 = truncate_for_display("\xF0\x9F\xA6\x80"
                                      "abc",
                                      5);
    EXPECT_STR_EQ(out2, "\xF0\x9F\xA6\x80"
                        "abc");
    free(out2);
}

static void test_truncate_null(void)
{
    char *out = truncate_for_display(NULL, 10);
    EXPECT_STR_EQ(out, "");
    free(out);
}

/* ---------- wrap_break_pos ---------- */

static void test_wrap_break_fits(void)
{
    /* Whole string fits: end == resume == len, no break. */
    size_t resume = 999;
    size_t end = wrap_break_pos("hello", 5, 10, &resume);
    EXPECT(end == 5);
    EXPECT(resume == 5);
}

static void test_wrap_break_at_space(void)
{
    /* "hello world" with width 10: rightmost space in [0..10] is at
     * byte 5. End = 5 (excludes the space), resume = 6 (skips it).
     * Row content is "hello"; next row starts at "world". */
    size_t resume = 0;
    size_t end = wrap_break_pos("hello world", 11, 10, &resume);
    EXPECT(end == 5);
    EXPECT(resume == 6);
}

static void test_wrap_break_at_boundary(void)
{
    /* A space sitting exactly at position max_cells is a valid break:
     * "hello world" with width 5 — s[5] is the space — yields row
     * "hello" with the boundary space consumed. Without this rule the
     * helper would hard-break "hello" mid-word and the leading space
     * would leak onto the next row. */
    size_t resume = 0;
    size_t end = wrap_break_pos("hello world", 11, 5, &resume);
    EXPECT(end == 5);
    EXPECT(resume == 6);
}

static void test_wrap_break_hard_split(void)
{
    /* No space anywhere in [0..max_cells]: hard-break at the column
     * boundary. Use a fixture where the only space sits well past the
     * window. */
    size_t resume = 0;
    size_t end = wrap_break_pos("helloworldmore stuff", 20, 10, &resume);
    EXPECT(end == 10);
    EXPECT(resume == 10);
}

static void test_wrap_break_trims_trailing_spaces(void)
{
    /* Defensive trim: if the input wasn't pre-flattened, a run of
     * spaces before the break point shouldn't leak into the row.
     * Input "hi  more" with width 5: the rightmost space in [0..5] is
     * at byte 3; the trim then walks end back over the space at byte
     * 2, landing end=2 (row "hi") with resume=4 (start of "more"). */
    size_t resume = 0;
    size_t end = wrap_break_pos("hi  more", 8, 5, &resume);
    EXPECT(end == 2);
    EXPECT(resume == 4);
}

static void test_wrap_break_null_resume(void)
{
    /* resume_at NULL is allowed for callers that don't need it. */
    size_t end = wrap_break_pos("hello world", 11, 10, NULL);
    EXPECT(end == 5);
}

static void test_wrap_break_multibyte_aware(void)
{
    /* "café world" — 10 cells, 11 bytes (é is 2 bytes, 1 cell).
     * With max_cells=4, the boundary space at column 4 is byte 5
     * (since é occupies bytes 3-4). Row content "café" (4 cells,
     * 5 bytes); resume at "world" (byte 6). A byte-counting
     * implementation would have broken mid-é. */
    size_t resume = 0;
    size_t end = wrap_break_pos("caf\xC3\xA9 world", 11, 4, &resume);
    EXPECT(end == 5);
    EXPECT(resume == 6);
}

static void test_wrap_break_keeps_combining_with_base(void)
{
    /* "abcd̃ef" — a, b, c, d+COMBINING TILDE, e, f. 6 cells, 8 bytes
     * (combining tilde U+0303 = CC 83). With max_cells=4 and no space,
     * the hard split should land *after* the combining mark so it
     * stays attached to its base 'd', not orphaned at the start of
     * the next row. */
    size_t resume = 0;
    size_t end = wrap_break_pos("abcd\xCC\x83"
                                "ef",
                                8, 4, &resume);
    EXPECT(end == 6);    /* "abcd̃" = 4 ASCII + 2-byte combining */
    EXPECT(resume == 6); /* hard-break, no space consumed */
}

static void test_wrap_break_oversized_first_codepoint(void)
{
    /* Pathological budget: max_cells=1, first codepoint is 2 cells
     * (emoji). No space anywhere in the window. Without forward-
     * progress protection, advance_cells would return 0 and stall
     * the caller's loop. Helper takes the emoji as a single-codepoint
     * row instead — visible row overflows by 1 cell, but the caller
     * still advances and the row count stays bounded. */
    size_t resume = 0;
    /* "🦀abc" — emoji (4 bytes) + abc. */
    size_t end = wrap_break_pos("\xF0\x9F\xA6\x80"
                                "abc",
                                7, 1, &resume);
    EXPECT(end == 4);    /* one emoji codepoint = 4 bytes */
    EXPECT(resume == 4); /* hard-break, no space consumed */
}

/* ---------- wrap_row_bytes ---------- */

static void test_wrap_row_fits(void)
{
    /* Whole string on one row: no separator to consume. */
    size_t separator = 999;
    size_t row = wrap_row_bytes("hello", 10, &separator);
    EXPECT(row == 5);
    EXPECT(separator == 0);
}

static void test_wrap_row_word_break(void)
{
    /* Break at the space; the separator is the space itself. */
    size_t separator = 0;
    size_t row = wrap_row_bytes("hello world", 10, &separator);
    EXPECT(row == 5);
    EXPECT(separator == 1);
}

static void test_wrap_row_hard_break(void)
{
    /* No space in the window: hard break consumes nothing. */
    size_t separator = 999;
    size_t row = wrap_row_bytes("helloworld", 6, &separator);
    EXPECT(row == 6);
    EXPECT(separator == 0);
}

static void test_wrap_row_newline_ends_row(void)
{
    /* An embedded newline ends the row early and is consumed. */
    size_t separator = 0;
    size_t row = wrap_row_bytes("hi\nthere", 10, &separator);
    EXPECT(row == 2);
    EXPECT(separator == 1);
}

static void test_wrap_row_empty_paragraph(void)
{
    /* A leading newline yields an empty row while still advancing. */
    size_t separator = 0;
    size_t row = wrap_row_bytes("\nrest", 10, &separator);
    EXPECT(row == 0);
    EXPECT(separator == 1);
}

static void test_wrap_row_iterates_to_end(void)
{
    /* Advancing by row + separator visits every row and terminates. */
    const char *text = "one two\nthree";
    const char *expected[] = {"one", "two", "three"};
    size_t n = 0;
    while (*text) {
        size_t separator;
        size_t row = wrap_row_bytes(text, 5, &separator);
        EXPECT(n < 3);
        EXPECT(row == strlen(expected[n]));
        EXPECT(strncmp(text, expected[n], row) == 0);
        text += row + separator;
        n++;
    }
    EXPECT(n == 3);
}

/* ---------- reflow_for_display ---------- */

static void test_reflow_no_wrap_needed(void)
{
    /* Fits on first row with reserve room: pass-through dup. */
    char *out = reflow_for_display("short", 80, 80, 3, 0);
    EXPECT_STR_EQ(out, "short");
    free(out);
}

static void test_reflow_wraps_at_word(void)
{
    /* Two rows of width 10. Input "hello world more" (16 chars):
     *   row 0 (cap 10): "hello"      ← break at space, "world more" remains
     *   row 1 (cap 10): "world more" ← fits exactly
     * No truncation. */
    char *out = reflow_for_display("hello world more", 10, 10, 2, 0);
    EXPECT_STR_EQ(out, "hello\nworld more");
    free(out);
}

static void test_reflow_truncates_when_out_of_rows(void)
{
    /* Two rows of width 10, input "hello world more here". After row 0
     * ("hello"), 15 chars remain ("world more here"); row 1 is the last
     * row so we reserve 3 for "..." and word-break in width-3=7. Window
     * "world m" — last space at byte 5, so row 1 = "world" + "...". */
    char *out = reflow_for_display("hello world more here", 10, 10, 2, 0);
    EXPECT_STR_EQ(out, "hello\nworld...");
    free(out);
}

static void test_reflow_long_unbreakable_word(void)
{
    /* No spaces in the input but still longer than one row: hard-break
     * mid-word, then truncate the last row. width=10, max_rows=2 →
     * row 0 = first 10 chars, row 1 = next 7 + "...". */
    char *out = reflow_for_display("abcdefghijklmnopqrstuvwxyz", 10, 10, 2, 0);
    EXPECT_STR_EQ(out, "abcdefghij\nklmnopq...");
    free(out);
}

static void test_reflow_unbreakable_across_3_rows_truncates(void)
{
    /* The pathological case: a long unbroken token that overflows even
     * a 3-row budget. Every row hard-breaks at the column boundary
     * (no space anywhere in the input), and the last row hard-cuts
     * at width-3 to leave room for the trailing "...". width=10,
     * max_rows=3, input 40 a's → rows 0 and 1 each take 10 chars,
     * row 2 takes 7 chars + "..." (10 cells visible). */
    char input[41];
    memset(input, 'a', 40);
    input[40] = '\0';
    char *out = reflow_for_display(input, 10, 10, 3, 0);
    EXPECT_STR_EQ(out, "aaaaaaaaaa\naaaaaaaaaa\naaaaaaa...");
    free(out);
}

static void test_reflow_unbreakable_fits_in_3_rows(void)
{
    /* Same shape but shorter — fits exactly in 3 rows of 10, no
     * truncation. Boundary case: 30 a's exactly fills the budget. */
    char input[31];
    memset(input, 'a', 30);
    input[30] = '\0';
    char *out = reflow_for_display(input, 10, 10, 3, 0);
    EXPECT_STR_EQ(out, "aaaaaaaaaa\naaaaaaaaaa\naaaaaaaaaa");
    free(out);
}

static void test_reflow_first_row_smaller(void)
{
    /* First row narrower than mid (caller has prefix). first_row=5,
     * mid_row=10, max_rows=2. "hello brave new world" (21):
     *   row 0 (cap 5):  "hello"        ← break at space at byte 5
     *   row 1 (cap 10, last): 15 chars "brave new world" remain; word-
     *     break in width-3=7 → window "brave n", last space at byte 5,
     *     row = "brave..." */
    char *out = reflow_for_display("hello brave new world", 5, 10, 2, 0);
    EXPECT_STR_EQ(out, "hello\nbrave...");
    free(out);
}

static void test_reflow_last_row_strict_for_wide_codepoint(void)
{
    /* The ellipsis must not force a wide codepoint beyond the row budget. */
    char *out = reflow_for_display("\xE7\x95\x8C"
                                   "xxx",
                                   4, 4, 1, 0);
    EXPECT_STR_EQ(out, "...");
    free(out);
}

static void test_reflow_reserve_applies_when_tail_fits_early(void)
{
    /* Every row leaves suffix space because it may become the last emitted row. */
    char *out = reflow_for_display("abcdef", 10, 10, 3, 5);
    EXPECT_STR_EQ(out, "abcde\nf");
    free(out);
}

static void test_reflow_last_row_reserve(void)
{
    /* last_row_reserve shrinks the LAST row's effective width so a
     * caller-appended suffix fits. Single-row mode (max_rows=1):
     * input "hello world" (11), first_row=11, reserve=4 → effective
     * width 7, doesn't fit → truncate. Width-3=4, window "hell", no
     * space → hard cut → "hell...". */
    char *out = reflow_for_display("hello world", 11, 11, 1, 4);
    EXPECT_STR_EQ(out, "hell...");
    free(out);
}

static void test_reflow_null_input(void)
{
    char *out = reflow_for_display(NULL, 80, 80, 3, 0);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_reflow_empty_input(void)
{
    char *out = reflow_for_display("", 80, 80, 3, 0);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_reflow_long_bash_command(void)
{
    const char *cmd =
        "find . -type f -name '*.c' -not -path './build/*' -not -path './build-asan/*' "
        "| xargs grep -l 'TODO' "
        "| head -20 "
        "| while read f; do echo \"== $f ==\"; grep -n TODO \"$f\"; done";

    char *out = reflow_for_display(cmd, 93, 100, 3, 0);

    /* Assert layout constraints rather than coupling this fixture to exact word breaks. */
    int rows = 1;
    for (const char *p = out; *p; p++)
        if (*p == '\n')
            rows++;
    EXPECT(rows <= 3);

    /* Each row fits its budget. */
    int row = 0;
    int row_len = 0;
    for (const char *p = out;; p++) {
        if (*p == '\n' || *p == '\0') {
            int budget = (row == 0) ? 93 : 100;
            if (row_len > budget)
                FAIL("row %d length %d exceeds budget %d", row, row_len, budget);
            if (*p == '\0')
                break;
            row++;
            row_len = 0;
        } else {
            row_len++;
        }
    }

    /* Row 0 starts with "find ", confirms the head of the input is
     * preserved verbatim (no stray truncation up front). */
    EXPECT(strncmp(out, "find . -type f", 14) == 0);
    free(out);
}

static void test_reflow_long_bash_command_truncated(void)
{
    const char *cmd = "find . -type f -name '*.c' -not -path './build/*' "
                      "| xargs grep -l TODO | head | while read f; do echo $f; done";

    char *out = reflow_for_display(cmd, 33, 40, 2, 0);

    /* Exactly 2 rows. */
    int newlines = 0;
    for (const char *p = out; *p; p++)
        if (*p == '\n')
            newlines++;
    EXPECT(newlines == 1);

    /* Last row ends with "..." since the input doesn't fit in 2 rows. */
    size_t n = strlen(out);
    EXPECT(n >= 3);
    EXPECT(memcmp(out + n - 3, "...", 3) == 0);

    /* Row 0 starts with the head of the command. */
    EXPECT(strncmp(out, "find . -type f", 14) == 0);
    free(out);
}

int main(void)
{
    /* Width helpers use utf8_codepoint_cells (mbrtowc + wcwidth) for cell-accurate
     * width — they need a UTF-8 LC_CTYPE. */
    locale_init_utf8();

    test_flatten_null();
    test_flatten_empty();
    test_flatten_plain();
    test_flatten_newline();
    test_flatten_collapses_runs();
    test_flatten_strips_edges();
    test_flatten_all_whitespace();
    test_flatten_control_bytes();
    test_display_cells();
    test_flatten_preserves_high_bytes();
    test_flatten_substitutes_bidi_override();
    test_flatten_substitutes_zwj();
    test_flatten_substitutes_malformed_utf8();
    test_flatten_caps_zero_width_run();
    test_flatten_preserves_legit_combining_run();

    test_truncate_under_cap();
    test_truncate_exact_cap();
    test_truncate_above_cap();
    test_truncate_tiny_cap();
    test_truncate_utf8_boundary();
    test_truncate_keeps_multibyte_intact();
    test_truncate_under_cap_multibyte();
    test_truncate_keeps_trailing_combining_mark();
    test_truncate_emoji_two_cells();
    test_truncate_null();

    test_wrap_break_fits();
    test_wrap_break_at_space();
    test_wrap_break_at_boundary();
    test_wrap_break_hard_split();
    test_wrap_break_trims_trailing_spaces();
    test_wrap_break_null_resume();
    test_wrap_break_multibyte_aware();
    test_wrap_break_keeps_combining_with_base();
    test_wrap_break_oversized_first_codepoint();

    test_wrap_row_fits();
    test_wrap_row_word_break();
    test_wrap_row_hard_break();
    test_wrap_row_newline_ends_row();
    test_wrap_row_empty_paragraph();
    test_wrap_row_iterates_to_end();

    test_reflow_no_wrap_needed();
    test_reflow_wraps_at_word();
    test_reflow_truncates_when_out_of_rows();
    test_reflow_long_unbreakable_word();
    test_reflow_unbreakable_across_3_rows_truncates();
    test_reflow_unbreakable_fits_in_3_rows();
    test_reflow_first_row_smaller();
    test_reflow_last_row_strict_for_wide_codepoint();
    test_reflow_reserve_applies_when_tail_fits_early();
    test_reflow_last_row_reserve();
    test_reflow_null_input();
    test_reflow_empty_input();
    test_reflow_long_bash_command();
    test_reflow_long_bash_command_truncated();

    T_REPORT();
}
