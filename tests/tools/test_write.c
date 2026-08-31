/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "harness.h"
#include "tool.h"
#include "xalloc.h"
#include "system/fs.h"

static char *call_write(const char *path, const char *content)
{
    json_t *arguments = json_pack("{s:s, s:s}", "path", path, "content", content);
    char *args_json = json_dumps(arguments, JSON_COMPACT);
    json_decref(arguments);
    char *result = TOOL_WRITE.run(args_json, NULL);
    free(args_json);
    return result;
}

static char *read_file(const char *path)
{
    size_t n = 0;
    return fs_read_file(path, &n);
}

static void test_write_invalid_json(void)
{
    char *out = TOOL_WRITE.run("not json", NULL);
    EXPECT(strstr(out, "invalid arguments") != NULL);
    free(out);
}

static void test_write_missing_path(void)
{
    char *out = TOOL_WRITE.run("{\"content\":\"x\"}", NULL);
    EXPECT(strstr(out, "missing 'path'") != NULL);
    free(out);
}

static void test_write_missing_content(void)
{
    char *out = TOOL_WRITE.run("{\"path\":\"/tmp/x\"}", NULL);
    EXPECT(strstr(out, "missing 'content'") != NULL);
    free(out);
}

static void test_write_creates_new_file(void)
{
    /* New-file output summarizes the arguments instead of echoing them as a diff. */
    char *dir = t_tempdir();
    char *path = xasprintf("%s/new.txt", dir);

    char *out = call_write(path, "alpha\nbeta\n");
    EXPECT(strstr(out, "created ") != NULL);
    EXPECT(strstr(out, path) != NULL);
    EXPECT(strstr(out, "2 lines") != NULL);
    EXPECT(strstr(out, "11 bytes") != NULL);
    EXPECT(strstr(out, "--- /dev/null") == NULL);
    EXPECT(strstr(out, "+alpha") == NULL);
    free(out);

    char *got = read_file(path);
    EXPECT_STR_EQ(got, "alpha\nbeta\n");
    free(got);

    free(path);
}

static void test_write_creates_parent_dirs(void)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/sub/deeper/file.txt", dir);

    char *out = call_write(path, "content\n");
    EXPECT(strstr(out, "created ") != NULL);
    free(out);

    char *got = read_file(path);
    EXPECT_STR_EQ(got, "content\n");
    free(got);

    free(path);
}

static void test_write_overwrites(void)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/file.txt", dir);

    char *out = call_write(path, "first\n");
    free(out);

    out = call_write(path, "second\n");
    EXPECT(strstr(out, "-first") != NULL);
    EXPECT(strstr(out, "+second") != NULL);
    free(out);

    char *got = read_file(path);
    EXPECT_STR_EQ(got, "second\n");
    free(got);

    free(path);
}

static void test_write_preserves_mode(void)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/script.sh", dir);

    char *out = call_write(path, "echo hi\n");
    free(out);
    chmod(path, 0750);

    out = call_write(path, "echo bye\n");
    free(out);

    struct stat st;
    EXPECT(stat(path, &st) == 0);
    EXPECT((st.st_mode & 0777) == 0750);

    free(path);
}

static void test_write_preserves_setuid(void)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/helper", dir);

    char *out = call_write(path, "old\n");
    free(out);
    EXPECT(chmod(path, 04755) == 0); /* setuid + rwxr-xr-x */

    out = call_write(path, "new\n");
    free(out);

    struct stat st;
    EXPECT(stat(path, &st) == 0);
    EXPECT((st.st_mode & 07777) == 04755);

    free(path);
}

static void test_write_unchanged_yields_empty_diff(void)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/file.txt", dir);

    char *out = call_write(path, "same\n");
    free(out);

    /* Unchanged writes must preserve inode identity and hard links. */
    struct stat before, after;
    EXPECT(stat(path, &before) == 0);

    out = call_write(path, "same\n");
    EXPECT_STR_EQ(out, "");
    free(out);

    EXPECT(stat(path, &after) == 0);
    EXPECT(before.st_ino == after.st_ino);

    free(path);
}

static void test_write_refuses_fifo(void)
{
    /* Reading a FIFO to generate a diff could block indefinitely. */
    char *dir = t_tempdir();
    char *path = xasprintf("%s/pipe", dir);
    EXPECT(mkfifo(path, 0644) == 0);

    char *out = call_write(path, "x\n");
    EXPECT(strstr(out, "not a regular file") != NULL);
    free(out);

    struct stat st;
    EXPECT(stat(path, &st) == 0);
    EXPECT(S_ISFIFO(st.st_mode));

    free(path);
}

static void test_write_through_dangling_symlink(void)
{
    char *dir = t_tempdir();
    char *real = xasprintf("%s/real.txt", dir);
    char *link = xasprintf("%s/link.txt", dir);

    EXPECT(symlink(real, link) == 0); /* dangling on purpose */

    char *out = call_write(link, "hello\n");
    EXPECT(strstr(out, "created ") != NULL);
    free(out);

    struct stat lst;
    EXPECT(lstat(link, &lst) == 0);
    EXPECT(S_ISLNK(lst.st_mode));

    char *got = read_file(real);
    EXPECT_STR_EQ(got, "hello\n");
    free(got);

    free(real);
    free(link);
}

static void test_write_empty_new_file(void)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/empty.txt", dir);

    char *out = call_write(path, "");
    EXPECT(strstr(out, "created ") != NULL);
    EXPECT(strstr(out, "(empty)") != NULL);
    free(out);

    struct stat st;
    EXPECT(stat(path, &st) == 0);
    EXPECT(st.st_size == 0);

    free(path);
}

static void test_write_blank_content_summary(void)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/blank.txt", dir);

    char *out = call_write(path, "\n   \n"); /* only blank lines */
    EXPECT(strstr(out, "created ") != NULL);
    EXPECT(strstr(out, "2 lines") != NULL);
    EXPECT(strstr(out, "5 bytes") != NULL);
    free(out);

    free(path);
}

static void test_write_through_symlink(void)
{
    char *dir = t_tempdir();
    char *real = xasprintf("%s/real.txt", dir);
    char *link = xasprintf("%s/link.txt", dir);

    char *out = call_write(real, "first\n");
    free(out);
    EXPECT(symlink(real, link) == 0);

    out = call_write(link, "second\n");
    EXPECT(strstr(out, "+second") != NULL);
    free(out);

    struct stat lst;
    EXPECT(lstat(link, &lst) == 0);
    EXPECT(S_ISLNK(lst.st_mode));

    char *got = read_file(real);
    EXPECT_STR_EQ(got, "second\n");
    free(got);

    free(real);
    free(link);
}

int main(void)
{
    test_write_invalid_json();
    test_write_missing_path();
    test_write_missing_content();
    test_write_creates_new_file();
    test_write_creates_parent_dirs();
    test_write_overwrites();
    test_write_preserves_mode();
    test_write_preserves_setuid();
    test_write_unchanged_yields_empty_diff();
    test_write_empty_new_file();
    test_write_blank_content_summary();
    test_write_through_symlink();
    test_write_through_dangling_symlink();
    test_write_refuses_fifo();
    T_REPORT();
}
