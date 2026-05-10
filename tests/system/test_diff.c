/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "system/diff.h"

static void test_diff_identical(void)
{
    char *out = make_unified_diff("hello\n", 6, "hello\n", 6, "a/foo", "b/foo");
    EXPECT(out != NULL);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_diff_simple_change(void)
{
    char *out = make_unified_diff("hello\nworld\n", 12, "hello\nthere\n", 12, "a/foo", "b/foo");
    EXPECT(out != NULL);
    /* Headers come first, then a hunk with -world / +there. */
    EXPECT(strstr(out, "--- a/foo") != NULL);
    EXPECT(strstr(out, "+++ b/foo") != NULL);
    EXPECT(strstr(out, "-world") != NULL);
    EXPECT(strstr(out, "+there") != NULL);
    free(out);
}

static void test_diff_new_file(void)
{
    /* Convention: a-side is /dev/null when the file is being created. */
    char *out = make_unified_diff("", 0, "alpha\nbeta\n", 11, "/dev/null", "b/new.txt");
    EXPECT(out != NULL);
    EXPECT(strstr(out, "--- /dev/null") != NULL);
    EXPECT(strstr(out, "+++ b/new.txt") != NULL);
    EXPECT(strstr(out, "+alpha") != NULL);
    EXPECT(strstr(out, "+beta") != NULL);
    free(out);
}

static void test_diff_delete_to_empty(void)
{
    char *out = make_unified_diff("only line\n", 10, "", 0, "a/old", "/dev/null");
    EXPECT(out != NULL);
    EXPECT(strstr(out, "-only line") != NULL);
    free(out);
}

static void test_diff_no_trailing_newline(void)
{
    /* GNU/BSD diff both emit "\ No newline at end of file" when one side
     * lacks the trailing newline. */
    char *out = make_unified_diff("a\nb", 3, "a\nb\n", 4, "a/foo", "b/foo");
    EXPECT(out != NULL);
    EXPECT(strstr(out, "No newline at end of file") != NULL);
    free(out);
}

static void test_diff_with_nul_bytes(void)
{
    /* Without -a, diff(1) would say "Binary files X and Y differ" for
     * any file containing a NUL — leaving the tool with a non-renderable
     * result. With -a it produces a regular unified diff, and the NULs
     * are subsequently scrubbed to U+FFFD by sanitize_utf8. */
    char old_buf[] = {'a', '\0', 'b', '\n'};
    const char *new_buf = "abc\n";
    char *out = make_unified_diff(old_buf, sizeof(old_buf), new_buf, 4, "a/f", "b/f");
    EXPECT(out != NULL);
    EXPECT(strstr(out, "--- a/f") != NULL); /* real unified diff, not "Binary files" */
    EXPECT(strstr(out, "Binary files") == NULL);
    for (size_t i = 0; out[i]; i++)
        EXPECT(out[i] != '\0');
    free(out);
}

static void test_diff_sanitizes_invalid_utf8(void)
{
    /* The "old" side has a raw 0xff byte (invalid UTF-8). diff(1) copies
     * it verbatim into the '-' line; without sanitization that byte would
     * survive into the tool result and break the next JSON request. */
    char old_buf[] = {'a', (char)0xff, 'b', '\n'};
    const char *new_buf = "abc\n";
    char *out = make_unified_diff(old_buf, sizeof(old_buf), new_buf, 4, "a/f", "b/f");
    EXPECT(out != NULL);
    /* No raw 0xff bytes pass through. */
    for (size_t i = 0; out[i]; i++)
        EXPECT((unsigned char)out[i] != 0xff);
    /* The replacement character (U+FFFD = EF BF BD) appears where the
     * invalid byte was. */
    EXPECT(strstr(out, "\xEF\xBF\xBD") != NULL);
    free(out);
}

int main(void)
{
    test_diff_identical();
    test_diff_simple_change();
    test_diff_new_file();
    test_diff_delete_to_empty();
    test_diff_no_trailing_newline();
    test_diff_sanitizes_invalid_utf8();
    test_diff_with_nul_bytes();
    T_REPORT();
}
