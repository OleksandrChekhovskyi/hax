/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "harness.h"
#include "render/diff_color.h"

static enum diff_line_kind classify(const char *line, int in_hunk)
{
    return diff_line_classify(line, strlen(line), in_hunk);
}

static void test_file_headers_before_hunk(void)
{
    EXPECT(classify("--- a/f.c", 0) == DIFF_LINE_META);
    EXPECT(classify("+++ b/f.c", 0) == DIFF_LINE_META);
    /* Bare absolute-path label form, used for files outside cwd. */
    EXPECT(classify("--- /abs/path", 0) == DIFF_LINE_META);
    EXPECT(diff_is_file_header("--- a/f.c", 9));
    EXPECT(diff_is_file_header("+++ b/f.c", 9));
    /* No trailing space → not a header. */
    EXPECT(!diff_is_file_header("---", 3));
    EXPECT(!diff_is_file_header("-- x", 4));
}

static void test_in_hunk_prefix_wins_over_header_shape(void)
{
    /* Inside a hunk the one-char prefix alone classifies: a removed
     * "-- x" renders as "--- x" and an added "++ x" as "+++ x" — real
     * content, never re-matched as file headers. */
    EXPECT(classify("--- x", 1) == DIFF_LINE_REMOVE);
    EXPECT(classify("+++ x", 1) == DIFF_LINE_ADD);
}

static void test_hunk_body_classification(void)
{
    EXPECT(classify("+new", 1) == DIFF_LINE_ADD);
    EXPECT(classify("-old", 1) == DIFF_LINE_REMOVE);
    EXPECT(classify(" context", 1) == DIFF_LINE_CONTEXT);
    /* One-char changed lines (empty add/remove) still classify. */
    EXPECT(classify("+", 1) == DIFF_LINE_ADD);
    EXPECT(classify("-", 1) == DIFF_LINE_REMOVE);
    /* A fully empty body line has no prefix — context. */
    EXPECT(classify("", 1) == DIFF_LINE_CONTEXT);
}

static void test_markers_meta_in_both_positions(void)
{
    /* "@@" and "\ No newline" match up front whether or not a hunk has
     * begun — an inter-hunk "@@" separator must not read as body. */
    EXPECT(classify("@@ -1 +1 @@", 0) == DIFF_LINE_META);
    EXPECT(classify("@@ -9 +9 @@", 1) == DIFF_LINE_META);
    EXPECT(classify("\\ No newline at end of file", 0) == DIFF_LINE_META);
    EXPECT(classify("\\ No newline at end of file", 1) == DIFF_LINE_META);
}

static void test_degenerate_diff_without_hunk_header(void)
{
    /* Pre-hunk fallback: stray +/- lines in input with no "@@" header
     * still read as changes; anything else is META. */
    EXPECT(classify("+new", 0) == DIFF_LINE_ADD);
    EXPECT(classify("-old", 0) == DIFF_LINE_REMOVE);
    EXPECT(classify("-- x", 0) == DIFF_LINE_REMOVE);
    EXPECT(classify("junk", 0) == DIFF_LINE_META);
    EXPECT(classify("", 0) == DIFF_LINE_META);
}

int main(void)
{
    test_file_headers_before_hunk();
    test_in_hunk_prefix_wins_over_header_shape();
    test_hunk_body_classification();
    test_markers_meta_in_both_positions();
    test_degenerate_diff_without_hunk_header();
    T_REPORT();
}
