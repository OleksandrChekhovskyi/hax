/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "harness.h"
#include "tool.h"
#include "util.h"

static char *mk_tmpdir(void)
{
    char *path = xstrdup("/tmp/hax-edit-XXXXXX");
    if (!mkdtemp(path)) {
        FAIL("mkdtemp: %s", strerror(errno));
        free(path);
        return NULL;
    }
    return path;
}

static void rm_rf(const char *dir)
{
    char *cmd = xasprintf("rm -rf '%s'", dir);
    (void)system(cmd);
    free(cmd);
}

static char *seed_file(const char *dir, const char *name, const char *content)
{
    char *path = xasprintf("%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    EXPECT(f != NULL);
    fputs(content, f);
    fclose(f);
    return path;
}

static char *slurp(const char *path)
{
    size_t n = 0;
    return slurp_file(path, &n);
}

static void test_edit_missing_path(void)
{
    char *out = TOOL_EDIT.run("{\"old_string\":\"a\",\"new_string\":\"b\"}", NULL, NULL);
    EXPECT(strstr(out, "missing 'path'") != NULL);
    free(out);
}

static void test_edit_missing_old(void)
{
    char *out = TOOL_EDIT.run("{\"path\":\"/tmp/x\",\"new_string\":\"b\"}", NULL, NULL);
    EXPECT(strstr(out, "missing 'old_string'") != NULL);
    free(out);
}

static void test_edit_missing_new(void)
{
    char *out = TOOL_EDIT.run("{\"path\":\"/tmp/x\",\"old_string\":\"a\"}", NULL, NULL);
    EXPECT(strstr(out, "missing 'new_string'") != NULL);
    free(out);
}

static void test_edit_old_empty(void)
{
    char *out =
        TOOL_EDIT.run("{\"path\":\"/tmp/x\",\"old_string\":\"\",\"new_string\":\"b\"}", NULL, NULL);
    EXPECT(strstr(out, "'old_string' must be non-empty") != NULL);
    free(out);
}

static void test_edit_identical_strings(void)
{
    char *out = TOOL_EDIT.run("{\"path\":\"/tmp/x\",\"old_string\":\"a\",\"new_string\":\"a\"}",
                              NULL, NULL);
    EXPECT(strstr(out, "identical") != NULL);
    free(out);
}

static void test_edit_unique_match(void)
{
    char *dir = mk_tmpdir();
    char *path = seed_file(dir, "f.txt", "alpha\nbeta\ngamma\n");

    char *args =
        xasprintf("{\"path\":\"%s\",\"old_string\":\"beta\",\"new_string\":\"BETA\"}", path);
    char *out = TOOL_EDIT.run(args, NULL, NULL);
    free(args);
    EXPECT(strstr(out, "-beta") != NULL);
    EXPECT(strstr(out, "+BETA") != NULL);
    free(out);

    char *got = slurp(path);
    EXPECT_STR_EQ(got, "alpha\nBETA\ngamma\n");
    free(got);

    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_edit_no_match(void)
{
    char *dir = mk_tmpdir();
    char *path = seed_file(dir, "f.txt", "alpha\n");

    char *args = xasprintf("{\"path\":\"%s\",\"old_string\":\"zzz\",\"new_string\":\"q\"}", path);
    char *out = TOOL_EDIT.run(args, NULL, NULL);
    free(args);
    EXPECT(strstr(out, "not found") != NULL);
    free(out);

    /* File untouched. */
    char *got = slurp(path);
    EXPECT_STR_EQ(got, "alpha\n");
    free(got);

    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_edit_multi_match_requires_replace_all(void)
{
    char *dir = mk_tmpdir();
    char *path = seed_file(dir, "f.txt", "foo\nfoo\nfoo\n");

    char *args = xasprintf("{\"path\":\"%s\",\"old_string\":\"foo\",\"new_string\":\"bar\"}", path);
    char *out = TOOL_EDIT.run(args, NULL, NULL);
    free(args);
    EXPECT(strstr(out, "matches 3 places") != NULL);
    free(out);

    /* Untouched. */
    char *got = slurp(path);
    EXPECT_STR_EQ(got, "foo\nfoo\nfoo\n");
    free(got);

    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_edit_replace_all(void)
{
    char *dir = mk_tmpdir();
    char *path = seed_file(dir, "f.txt", "foo\nfoo\nfoo\n");

    char *args = xasprintf(
        "{\"path\":\"%s\",\"old_string\":\"foo\",\"new_string\":\"bar\",\"replace_all\":true}",
        path);
    char *out = TOOL_EDIT.run(args, NULL, NULL);
    free(args);
    EXPECT(strstr(out, "+bar") != NULL);
    free(out);

    char *got = slurp(path);
    EXPECT_STR_EQ(got, "bar\nbar\nbar\n");
    free(got);

    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_edit_multiline_match(void)
{
    char *dir = mk_tmpdir();
    char *path = seed_file(dir, "f.c", "int main(void)\n{\n\treturn 0;\n}\n");

    char *args = xasprintf(
        "{\"path\":\"%s\",\"old_string\":\"\\treturn 0;\\n\",\"new_string\":\"\\treturn 42;\\n\"}",
        path);
    char *out = TOOL_EDIT.run(args, NULL, NULL);
    free(args);
    EXPECT(strstr(out, "-\treturn 0;") != NULL);
    EXPECT(strstr(out, "+\treturn 42;") != NULL);
    free(out);

    char *got = slurp(path);
    EXPECT_STR_EQ(got, "int main(void)\n{\n\treturn 42;\n}\n");
    free(got);

    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_edit_refuses_fifo(void)
{
    /* slurp_file_capped on a FIFO without a writer would block the
     * agent forever. The tool must refuse upfront with a clear error. */
    char *dir = mk_tmpdir();
    char *path = xasprintf("%s/pipe", dir);
    EXPECT(mkfifo(path, 0644) == 0);

    char *args = xasprintf("{\"path\":\"%s\",\"old_string\":\"a\",\"new_string\":\"b\"}", path);
    char *out = TOOL_EDIT.run(args, NULL, NULL);
    free(args);
    EXPECT(strstr(out, "not a regular file") != NULL);
    free(out);

    struct stat st;
    EXPECT(stat(path, &st) == 0);
    EXPECT(S_ISFIFO(st.st_mode));

    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_edit_nonexistent_file(void)
{
    char *out = TOOL_EDIT.run("{\"path\":\"/nonexistent/hax/edit/path\",\"old_string\":\"a\","
                              "\"new_string\":\"b\"}",
                              NULL, NULL);
    EXPECT(strstr(out, "error reading") != NULL);
    free(out);
}

int main(void)
{
    test_edit_missing_path();
    test_edit_missing_old();
    test_edit_missing_new();
    test_edit_old_empty();
    test_edit_identical_strings();
    test_edit_unique_match();
    test_edit_no_match();
    test_edit_multi_match_requires_replace_all();
    test_edit_replace_all();
    test_edit_multiline_match();
    test_edit_refuses_fifo();
    test_edit_nonexistent_file();
    T_REPORT();
}
