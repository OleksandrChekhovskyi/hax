/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "text/diff.h"

static char *diff_str(const char *a, const char *b, const char *a_label, const char *b_label)
{
    return make_unified_diff(a, strlen(a), b, strlen(b), a_label, b_label);
}

static size_t count_occurrences(const char *haystack, const char *needle)
{
    size_t count = 0;
    for (const char *at = strstr(haystack, needle); at; at = strstr(at + 1, needle))
        count++;
    return count;
}

static void test_identical(void)
{
    char *out = diff_str("hello\n", "hello\n", "a/foo", "b/foo");
    EXPECT_STR_EQ(out, "");
    free(out);

    out = diff_str("", "", "a/foo", "b/foo");
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_simple_change(void)
{
    char *out = diff_str("hello\nworld\n", "hello\nthere\n", "a/foo", "b/foo");
    EXPECT_STR_EQ(out, "--- a/foo\n"
                       "+++ b/foo\n"
                       "@@ -1,2 +1,2 @@\n"
                       " hello\n"
                       "-world\n"
                       "+there\n");
    free(out);
}

static void test_single_line_change_omits_count(void)
{
    char *out = diff_str("a\n", "b\n", "a/foo", "b/foo");
    EXPECT_STR_EQ(out, "--- a/foo\n"
                       "+++ b/foo\n"
                       "@@ -1 +1 @@\n"
                       "-a\n"
                       "+b\n");
    free(out);
}

static void test_new_file(void)
{
    /* Convention: a-side is /dev/null when the file is being created. */
    char *out = diff_str("", "alpha\nbeta\n", "/dev/null", "b/new.txt");
    EXPECT_STR_EQ(out, "--- /dev/null\n"
                       "+++ b/new.txt\n"
                       "@@ -0,0 +1,2 @@\n"
                       "+alpha\n"
                       "+beta\n");
    free(out);
}

static void test_delete_to_empty(void)
{
    char *out = diff_str("only line\n", "", "a/old", "/dev/null");
    EXPECT_STR_EQ(out, "--- a/old\n"
                       "+++ /dev/null\n"
                       "@@ -1 +0,0 @@\n"
                       "-only line\n");
    free(out);
}

static void test_no_trailing_newline_old(void)
{
    char *out = diff_str("a\nb", "a\nb\n", "a/foo", "b/foo");
    EXPECT_STR_EQ(out, "--- a/foo\n"
                       "+++ b/foo\n"
                       "@@ -1,2 +1,2 @@\n"
                       " a\n"
                       "-b\n"
                       "\\ No newline at end of file\n"
                       "+b\n");
    free(out);
}

static void test_no_trailing_newline_new(void)
{
    char *out = diff_str("a\nb\n", "a\nb", "a/foo", "b/foo");
    EXPECT_STR_EQ(out, "--- a/foo\n"
                       "+++ b/foo\n"
                       "@@ -1,2 +1,2 @@\n"
                       " a\n"
                       "-b\n"
                       "+b\n"
                       "\\ No newline at end of file\n");
    free(out);
}

static void test_no_trailing_newline_context(void)
{
    char *out = diff_str("a\nz", "b\nz", "a/foo", "b/foo");
    EXPECT_STR_EQ(out, "--- a/foo\n"
                       "+++ b/foo\n"
                       "@@ -1,2 +1,2 @@\n"
                       "-a\n"
                       "+b\n"
                       " z\n"
                       "\\ No newline at end of file\n");
    free(out);
}

static void test_context_clipped_at_file_edges(void)
{
    char *out = diff_str("1\n2\n3\n4\n5\n", "one\n2\n3\n4\n5\n", "a/f", "b/f");
    EXPECT_STR_EQ(out, "--- a/f\n"
                       "+++ b/f\n"
                       "@@ -1,4 +1,4 @@\n"
                       "-1\n"
                       "+one\n"
                       " 2\n"
                       " 3\n"
                       " 4\n");
    free(out);
}

static void test_nearby_changes_share_hunk(void)
{
    char *out = diff_str("1\n2\n3\n4\n5\n", "1\nB\n3\nD\n5\n", "a/f", "b/f");
    EXPECT_STR_EQ(out, "--- a/f\n"
                       "+++ b/f\n"
                       "@@ -1,5 +1,5 @@\n"
                       " 1\n"
                       "-2\n"
                       "+B\n"
                       " 3\n"
                       "-4\n"
                       "+D\n"
                       " 5\n");
    free(out);
}

static void test_hunk_split_by_context_gap(void)
{
    /* Changes 2 * CONTEXT_LINES apart share overlapping context and must merge into one hunk;
     * one line further they split. */
    const char *base = "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n";

    char *out = diff_str(base, "one\n2\n3\n4\n5\n6\n7\neight\n9\n10\n11\n12\n", "a/f", "b/f");
    EXPECT(count_occurrences(out, "@@ -") == 1);
    free(out);

    out = diff_str(base, "one\n2\n3\n4\n5\n6\n7\n8\nnine\n10\n11\n12\n", "a/f", "b/f");
    EXPECT(count_occurrences(out, "@@ -") == 2);
    EXPECT(strstr(out, "@@ -1,4 +1,4 @@") != NULL);
    EXPECT(strstr(out, "@@ -6,7 +6,7 @@") != NULL);
    free(out);
}

static void test_inserted_block_aligns_with_blank_line(void)
{
    /* The insertion could equally start at the "}" or the blank line; the readable form keeps
     * the new function as one block after the blank separator. */
    char *out = diff_str("void a()\n{\n}\n\nvoid c()\n{\n}\n",
                         "void a()\n{\n}\n\nvoid b()\n{\n}\n\nvoid c()\n{\n}\n", "a/f", "b/f");
    EXPECT_STR_EQ(out, "--- a/f\n"
                       "+++ b/f\n"
                       "@@ -2,6 +2,10 @@\n"
                       " {\n"
                       " }\n"
                       " \n"
                       "+void b()\n"
                       "+{\n"
                       "+}\n"
                       "+\n"
                       " void c()\n"
                       " {\n"
                       " }\n");
    free(out);
}

static void test_deleted_block_aligns_with_blank_line(void)
{
    char *out = diff_str("void a()\n{\n}\n\nvoid b()\n{\n}\n\nvoid c()\n{\n}\n",
                         "void a()\n{\n}\n\nvoid c()\n{\n}\n", "a/f", "b/f");
    EXPECT_STR_EQ(out, "--- a/f\n"
                       "+++ b/f\n"
                       "@@ -2,10 +2,6 @@\n"
                       " {\n"
                       " }\n"
                       " \n"
                       "-void b()\n"
                       "-{\n"
                       "-}\n"
                       "-\n"
                       " void c()\n"
                       " {\n"
                       " }\n");
    free(out);
}

static void test_appended_duplicate_slides_to_bottom(void)
{
    char *out = diff_str("a\nb\n", "a\nb\nb\n", "a/f", "b/f");
    EXPECT_STR_EQ(out, "--- a/f\n"
                       "+++ b/f\n"
                       "@@ -1,2 +1,3 @@\n"
                       " a\n"
                       " b\n"
                       "+b\n");
    free(out);
}

static void test_with_nul_bytes(void)
{
    /* A NUL must not derail line splitting, and must leave the output as a regular unified diff
     * with the NUL scrubbed to U+FFFD. */
    char old_buf[] = {'a', '\0', 'b', '\n'};
    const char *new_buf = "abc\n";
    char *out = make_unified_diff(old_buf, sizeof(old_buf), new_buf, 4, "a/f", "b/f");
    EXPECT(strstr(out, "--- a/f") != NULL);
    EXPECT(strstr(out, "+abc") != NULL);
    for (size_t i = 0; out[i]; i++)
        EXPECT(out[i] != '\0');
    EXPECT(strstr(out, "\xEF\xBF\xBD") != NULL);
    free(out);
}

static void test_sanitizes_invalid_utf8(void)
{
    /* The "old" side has a raw 0xff byte (invalid UTF-8) that is copied verbatim into the '-'
     * line; without sanitization it would survive into the tool result and break the next JSON
     * request. */
    char old_buf[] = {'a', (char)0xff, 'b', '\n'};
    const char *new_buf = "abc\n";
    char *out = make_unified_diff(old_buf, sizeof(old_buf), new_buf, 4, "a/f", "b/f");
    for (size_t i = 0; out[i]; i++)
        EXPECT((unsigned char)out[i] != 0xff);
    EXPECT(strstr(out, "\xEF\xBF\xBD") != NULL);
    free(out);
}

static void test_sparse_edits_stay_local(void)
{
    /* Scattered single-line edits in a large file must come out as one small hunk each, not
     * degrade into a whole-file replacement when the search budget tightens. */
    enum { line_count = 40000, edit_stride = 2000 };
    char *old_text = malloc(line_count * 16);
    char *new_text = malloc(line_count * 16);
    size_t old_len = 0;
    size_t new_len = 0;
    for (int i = 0; i < line_count; i++) {
        char line[16];
        int len = snprintf(line, sizeof(line), "l%d\n", i);
        memcpy(old_text + old_len, line, (size_t)len);
        old_len += (size_t)len;
        if (i % edit_stride == 500)
            len = snprintf(line, sizeof(line), "e%d\n", i);
        memcpy(new_text + new_len, line, (size_t)len);
        new_len += (size_t)len;
    }

    char *out = make_unified_diff(old_text, old_len, new_text, new_len, "a/f", "b/f");
    EXPECT(count_occurrences(out, "@@ -") == line_count / edit_stride);
    EXPECT(strstr(out, "@@ -498,7 +498,7 @@\n") != NULL);
    EXPECT(strstr(out, "-l500\n") != NULL);
    EXPECT(strstr(out, "+e500\n") != NULL);
    EXPECT(strstr(out, "-l38500\n") != NULL);
    EXPECT(strlen(out) < 4096);
    free(out);
    free(old_text);
    free(new_text);
}

static void test_line_numbers_after_long_common_prefix(void)
{
    /* A change past the materialized-window slack exercises the line-number base that maps
     * window-relative hunks back to whole-file numbering. */
    char *old_text = malloc(300 * 8 + 8 + 300 * 8);
    char *new_text = malloc(300 * 8 + 8 + 300 * 8);
    size_t old_len = 0;
    size_t new_len = 0;
    for (int i = 0; i < 300; i++) {
        char line[8];
        int len = snprintf(line, sizeof(line), "x%d\n", i);
        memcpy(old_text + old_len, line, (size_t)len);
        old_len += (size_t)len;
        memcpy(new_text + new_len, line, (size_t)len);
        new_len += (size_t)len;
    }
    memcpy(old_text + old_len, "old\n", 4);
    old_len += 4;
    memcpy(new_text + new_len, "new\n", 4);
    new_len += 4;
    for (int i = 0; i < 300; i++) {
        char line[8];
        int len = snprintf(line, sizeof(line), "y%d\n", i);
        memcpy(old_text + old_len, line, (size_t)len);
        old_len += (size_t)len;
        memcpy(new_text + new_len, line, (size_t)len);
        new_len += (size_t)len;
    }

    char *out = make_unified_diff(old_text, old_len, new_text, new_len, "a/f", "b/f");
    EXPECT(count_occurrences(out, "@@ -") == 1);
    EXPECT(strstr(out, "@@ -298,7 +298,7 @@\n"
                       " x297\n"
                       " x298\n"
                       " x299\n"
                       "-old\n"
                       "+new\n"
                       " y0\n"
                       " y1\n"
                       " y2\n") != NULL);
    free(out);
    free(old_text);
    free(new_text);
}

static void test_no_newline_change_after_long_common_prefix(void)
{
    /* The changed final line has no shared suffix, so the window runs to EOF and the missing
     * newline must survive the windowing. */
    char *old_text = malloc(200 * 8 + 8);
    char *new_text = malloc(200 * 8 + 8);
    size_t old_len = 0;
    size_t new_len = 0;
    for (int i = 0; i < 200; i++) {
        char line[8];
        int len = snprintf(line, sizeof(line), "p%d\n", i);
        memcpy(old_text + old_len, line, (size_t)len);
        old_len += (size_t)len;
        memcpy(new_text + new_len, line, (size_t)len);
        new_len += (size_t)len;
    }
    memcpy(old_text + old_len, "aaa", 3);
    old_len += 3;
    memcpy(new_text + new_len, "bbb", 3);
    new_len += 3;

    char *out = make_unified_diff(old_text, old_len, new_text, new_len, "a/f", "b/f");
    EXPECT(strstr(out, "@@ -198,4 +198,4 @@\n") != NULL);
    EXPECT(strstr(out, " p199\n"
                       "-aaa\n"
                       "\\ No newline at end of file\n"
                       "+bbb\n"
                       "\\ No newline at end of file\n") != NULL);
    free(out);
    free(old_text);
    free(new_text);
}

static void test_large_rewrite_becomes_one_replacement(void)
{
    /* 3000 total lines with nothing in common repeatedly exhaust region step budgets; the
     * pieces must still coalesce into a single whole-file replacement hunk instead of hanging
     * or exhausting memory. */
    enum { line_count = 1500 };
    char *old_text = malloc(line_count * 16);
    char *new_text = malloc(line_count * 16);
    size_t old_len = 0;
    size_t new_len = 0;
    for (int i = 0; i < line_count; i++) {
        old_len += (size_t)snprintf(old_text + old_len, 16, "old%d\n", i);
        new_len += (size_t)snprintf(new_text + new_len, 16, "new%d\n", i);
    }

    char *out = make_unified_diff(old_text, old_len, new_text, new_len, "a/f", "b/f");
    EXPECT(strstr(out, "@@ -1,1500 +1,1500 @@\n") != NULL);
    EXPECT(count_occurrences(out, "@@ -") == 1);
    EXPECT(strstr(out, "-old0\n") != NULL);
    EXPECT(strstr(out, "-old1499\n") != NULL);
    EXPECT(strstr(out, "+new0\n") != NULL);
    EXPECT(strstr(out, "+new1499\n") != NULL);
    free(out);
    free(old_text);
    free(new_text);
}

int main(void)
{
    test_identical();
    test_simple_change();
    test_single_line_change_omits_count();
    test_new_file();
    test_delete_to_empty();
    test_no_trailing_newline_old();
    test_no_trailing_newline_new();
    test_no_trailing_newline_context();
    test_context_clipped_at_file_edges();
    test_nearby_changes_share_hunk();
    test_hunk_split_by_context_gap();
    test_inserted_block_aligns_with_blank_line();
    test_deleted_block_aligns_with_blank_line();
    test_appended_duplicate_slides_to_bottom();
    test_with_nul_bytes();
    test_sanitizes_invalid_utf8();
    test_sparse_edits_stay_local();
    test_line_numbers_after_long_common_prefix();
    test_no_newline_change_after_long_common_prefix();
    test_large_rewrite_becomes_one_replacement();
    T_REPORT();
}
