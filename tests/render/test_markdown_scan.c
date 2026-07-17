/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "harness.h"
#include "render/markdown_scan.h"

struct expected_line {
    enum md_line_kind kind;
    size_t indent_length;
    size_t marker_length;
    char marker;
    int classification_complete;
    int normalize_indent;
};

static void expect_line(const char *input, int final, struct expected_line expected)
{
    struct md_line_info got = md_scan_line(input, strlen(input), final);
    EXPECT(got.kind == expected.kind);
    EXPECT(got.indent_length == expected.indent_length);
    EXPECT(got.marker_length == expected.marker_length);
    EXPECT(got.marker == expected.marker);
    EXPECT(got.classification_complete == expected.classification_complete);
    EXPECT(got.normalize_indent == expected.normalize_indent);
}

static void test_blank_and_text_lines(void)
{
    /* Whitespace waits for a possible blank-line newline; ordinary text is immediately decisive. */
    expect_line("", 0, (struct expected_line){MD_LINE_INCOMPLETE, 0, 0, 0, 0, 0});
    expect_line("   ", 0, (struct expected_line){MD_LINE_INCOMPLETE, 3, 0, 0, 0, 0});
    expect_line(" \t\n", 0, (struct expected_line){MD_LINE_BLANK, 2, 0, 0, 1, 1});
    expect_line("plain", 0, (struct expected_line){MD_LINE_TEXT, 0, 0, 0, 1, 0});
    expect_line("    # heading\n", 0, (struct expected_line){MD_LINE_TEXT, 4, 0, 0, 1, 0});
}

static void test_heading_and_fence_lines(void)
{
    /* Prefixes defer until validity is known; complete block markers normalize indentation. */
    expect_line("#", 0, (struct expected_line){MD_LINE_INCOMPLETE, 0, 0, 0, 0, 0});
    expect_line("  ## heading\n", 0, (struct expected_line){MD_LINE_HEADING, 2, 2, '#', 1, 1});
    expect_line("######\n", 0, (struct expected_line){MD_LINE_HEADING, 0, 6, '#', 1, 1});
    expect_line("#######\n", 0, (struct expected_line){MD_LINE_TEXT, 0, 0, 0, 1, 0});
    expect_line("##not\n", 0, (struct expected_line){MD_LINE_TEXT, 0, 0, 0, 1, 0});
    expect_line("``", 0, (struct expected_line){MD_LINE_INCOMPLETE, 0, 0, 0, 0, 0});
    expect_line("```", 0, (struct expected_line){MD_LINE_FENCE, 0, 3, '`', 0, 1});
    expect_line(" ```c\n", 0, (struct expected_line){MD_LINE_FENCE, 1, 3, '`', 1, 1});
}

static void test_thematic_and_list_lines(void)
{
    /* A bullet can force an eager block break while remaining open to thematic reclassification. */
    expect_line("* ", 0, (struct expected_line){MD_LINE_BULLET, 0, 1, '*', 0, 0});
    expect_line("* item", 0, (struct expected_line){MD_LINE_BULLET, 0, 1, '*', 1, 0});
    expect_line("+ ", 0, (struct expected_line){MD_LINE_BULLET, 0, 1, '+', 1, 0});
    expect_line("  * * *\n", 0, (struct expected_line){MD_LINE_THEMATIC, 2, 3, '*', 1, 1});
    expect_line("---\n", 0, (struct expected_line){MD_LINE_THEMATIC, 0, 3, '-', 1, 1});
    expect_line("---", 0, (struct expected_line){MD_LINE_INCOMPLETE, 0, 0, 0, 0, 0});
    expect_line("---x\n", 0, (struct expected_line){MD_LINE_TEXT, 0, 0, 0, 1, 0});
    expect_line("    * item", 0, (struct expected_line){MD_LINE_BULLET, 4, 1, '*', 1, 0});
    expect_line("\t* item", 0, (struct expected_line){MD_LINE_TEXT, 1, 0, 0, 1, 0});
}

static void test_ordered_list_lines(void)
{
    /* Ordered markers preserve nesting indent and defer while the delimiter remains ambiguous. */
    expect_line("  12) item", 0, (struct expected_line){MD_LINE_ORDERED, 2, 3, ')', 1, 0});
    expect_line("    1. item", 0, (struct expected_line){MD_LINE_ORDERED, 4, 2, '.', 1, 0});
    expect_line("1", 0, (struct expected_line){MD_LINE_INCOMPLETE, 0, 0, 0, 0, 0});
    expect_line("1x", 0, (struct expected_line){MD_LINE_INCOMPLETE, 0, 0, 0, 0, 0});
    expect_line("1xy", 0, (struct expected_line){MD_LINE_TEXT, 0, 0, 0, 1, 0});
}

static void test_blockquote_and_pipe_lines(void)
{
    /* Non-nesting block markers normalize only CommonMark's zero-to-three-space indent. */
    expect_line("  > quote", 0, (struct expected_line){MD_LINE_BLOCKQUOTE, 2, 1, '>', 1, 1});
    expect_line("   | A |", 0, (struct expected_line){MD_LINE_PIPE, 3, 1, '|', 1, 1});
    expect_line("    > quote", 0, (struct expected_line){MD_LINE_TEXT, 4, 0, 0, 1, 0});
}

static void test_final_lines(void)
{
    /* EOF resolves ambiguous prefixes literally while retaining markers already known to be valid.
     */
    expect_line("", 1, (struct expected_line){MD_LINE_TEXT, 0, 0, 0, 1, 0});
    expect_line("  ", 1, (struct expected_line){MD_LINE_TEXT, 2, 0, 0, 1, 0});
    expect_line("#", 1, (struct expected_line){MD_LINE_TEXT, 0, 0, 0, 1, 0});
    expect_line("```", 1, (struct expected_line){MD_LINE_TEXT, 0, 0, 0, 1, 0});
    expect_line("---", 1, (struct expected_line){MD_LINE_THEMATIC, 0, 3, '-', 1, 1});
    expect_line("===", 1, (struct expected_line){MD_LINE_THEMATIC, 0, 3, '=', 1, 1});
    expect_line("* ", 1, (struct expected_line){MD_LINE_TEXT, 0, 0, 0, 1, 0});
    expect_line("+ ", 1, (struct expected_line){MD_LINE_BULLET, 0, 1, '+', 1, 0});
    expect_line("1. ", 1, (struct expected_line){MD_LINE_ORDERED, 0, 2, '.', 1, 0});
    expect_line("1.", 1, (struct expected_line){MD_LINE_TEXT, 0, 0, 0, 1, 0});
}

int main(void)
{
    test_blank_and_text_lines();
    test_heading_and_fence_lines();
    test_thematic_and_list_lines();
    test_ordered_list_lines();
    test_blockquote_and_pipe_lines();
    test_final_lines();

    T_REPORT();
}
