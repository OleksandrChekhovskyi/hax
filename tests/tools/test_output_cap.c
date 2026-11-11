/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "tools/output_cap.h"

static void test_cap_lines_no_long_lines(void)
{
    /* Short lines fall through unchanged. */
    const char in[] = "hello\nworld\n";
    size_t n = 0;
    char *out = cap_line_lengths(in, sizeof(in) - 1, 100, &n);
    EXPECT(n == sizeof(in) - 1);
    EXPECT_STR_EQ(out, in);
    free(out);
}

static void test_cap_lines_empty(void)
{
    size_t n = 99;
    char *out = cap_line_lengths("", 0, 100, &n);
    EXPECT(out != NULL);
    EXPECT(n == 0);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_cap_lines_truncates_long_line(void)
{
    /* 10 'x' bytes, capped at 4: keep first 4 + marker, then trailing nl. */
    size_t n = 0;
    char *out = cap_line_lengths("xxxxxxxxxx\n", 11, 4, &n);
    EXPECT(strstr(out, "xxxx") == out); /* starts with 4 x's */
    EXPECT(strstr(out, "[6 bytes elided]") != NULL);
    EXPECT(out[n - 1] == '\n');
    free(out);
}

static void test_cap_lines_preserves_short_neighbors(void)
{
    /* Long line in the middle gets truncated; surrounding short lines
     * are left intact and the line count is preserved. */
    char in[3100];
    int written = snprintf(in, sizeof(in), "before\n");
    memset(in + written, 'y', 2500);
    written += 2500;
    in[written++] = '\n';
    written += snprintf(in + written, sizeof(in) - written, "after\n");
    size_t n = 0;
    char *out = cap_line_lengths(in, written, 1000, &n);
    EXPECT(strstr(out, "before\n") == out);
    EXPECT(strstr(out, "\nafter\n") != NULL);
    EXPECT(strstr(out, "[1500 bytes elided]") != NULL);
    /* Three lines in, three out. */
    int nl = 0;
    for (size_t i = 0; i < n; i++)
        if (out[i] == '\n')
            nl++;
    EXPECT(nl == 3);
    free(out);
}

static void test_cap_lines_long_line_no_trailing_newline(void)
{
    /* Last "line" has no terminator; cap still applies and the result
     * has no extra newline appended. */
    size_t n = 0;
    char *out = cap_line_lengths("zzzzzzzz", 8, 3, &n);
    EXPECT(strstr(out, "zzz") == out);
    EXPECT(strstr(out, "[5 bytes elided]") != NULL);
    EXPECT(out[n - 1] != '\n');
    free(out);
}

static void test_whole_lines_head(void)
{
    size_t lines = 99;
    EXPECT(whole_lines_head("a\nbb\ncc", 8, 10, &lines) == 5); /* the partial last line goes */
    EXPECT(lines == 2);
    EXPECT(whole_lines_head("a\nbb\ncc", 8, 1, &lines) == 2);
    EXPECT(lines == 1);
    EXPECT(whole_lines_head("long line", 9, 10, &lines) == 0);
    EXPECT(lines == 0);
    EXPECT(whole_lines_head(NULL, 0, 10, NULL) == 0);
}

static void test_whole_lines_tail(void)
{
    size_t lines = 99;
    /* Mid-line, the partial first line goes; a final unterminated line counts. */
    EXPECT(whole_lines_tail("aa\nbb\ncc", 8, 1, 10, &lines) == 3);
    EXPECT(lines == 2);
    EXPECT(whole_lines_tail("aa\nbb\ncc", 8, 0, 10, &lines) == 0);
    EXPECT(lines == 3);
    EXPECT(whole_lines_tail("aa\nbb\ncc", 8, 0, 1, &lines) == 6);
    EXPECT(lines == 1);
    /* A single long line is kept raw rather than erased. */
    EXPECT(whole_lines_tail("long line\n", 10, 1, 10, &lines) == 0);
    EXPECT(lines == 1);
    EXPECT(whole_lines_tail("long line", 9, 1, 10, &lines) == 0);
    EXPECT(lines == 1);
    EXPECT(whole_lines_tail(NULL, 0, 1, 10, &lines) == 0);
    EXPECT(lines == 0);
}

int main(void)
{
    test_cap_lines_no_long_lines();
    test_cap_lines_empty();
    test_cap_lines_truncates_long_line();
    test_cap_lines_preserves_short_neighbors();
    test_cap_lines_long_line_no_trailing_newline();
    test_whole_lines_head();
    test_whole_lines_tail();

    T_REPORT();
}
