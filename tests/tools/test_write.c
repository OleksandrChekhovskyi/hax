/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "harness.h"
#include "tool.h"
#include "util.h"

static char *mk_tmpdir(void)
{
    char *path = xstrdup("/tmp/hax-write-XXXXXX");
    if (!mkdtemp(path)) {
        FAIL("mkdtemp: %s", strerror(errno));
        free(path);
        return NULL;
    }
    return path;
}

static void rm_rf(const char *dir)
{
    /* Tests stay shallow — a single `rm -rf` via shell is fine. */
    char *cmd = xasprintf("rm -rf '%s'", dir);
    (void)system(cmd);
    free(cmd);
}

static char *call_write(const char *path, const char *content)
{
    char *cesc = xasprintf("%s", content);
    /* Build JSON with jansson? No — content has no quotes/backslashes in
     * our tests. Be careful when adding a test that does. */
    char *args = xasprintf("{\"path\":\"%s\",\"content\":\"%s\"}", path, cesc);
    free(cesc);
    char *out = TOOL_WRITE.run(args, NULL, NULL);
    free(args);
    return out;
}

static char *slurp(const char *path)
{
    size_t n = 0;
    return slurp_file(path, &n);
}

static void test_write_invalid_json(void)
{
    char *out = TOOL_WRITE.run("not json", NULL, NULL);
    EXPECT(strstr(out, "invalid arguments") != NULL);
    free(out);
}

static void test_write_missing_path(void)
{
    char *out = TOOL_WRITE.run("{\"content\":\"x\"}", NULL, NULL);
    EXPECT(strstr(out, "missing 'path'") != NULL);
    free(out);
}

static void test_write_missing_content(void)
{
    char *out = TOOL_WRITE.run("{\"path\":\"/tmp/x\"}", NULL, NULL);
    EXPECT(strstr(out, "missing 'content'") != NULL);
    free(out);
}

static void test_write_creates_new_file(void)
{
    /* New-file writes return a short "created <path> (...)" confirmation
     * rather than a full diff: the content is already in the model's tool
     * call arguments, so echoing it back as a `+`-prefixed diff would just
     * double the context cost. The diff is reserved for overwrites, where
     * it conveys real new-vs-old signal. */
    char *dir = mk_tmpdir();
    char *path = xasprintf("%s/new.txt", dir);

    char *out = call_write(path, "alpha\\nbeta\\n");
    EXPECT(strstr(out, "created ") != NULL);
    EXPECT(strstr(out, path) != NULL);
    EXPECT(strstr(out, "2 lines") != NULL);
    EXPECT(strstr(out, "11 bytes") != NULL);
    /* No diff content for new files. */
    EXPECT(strstr(out, "--- /dev/null") == NULL);
    EXPECT(strstr(out, "+alpha") == NULL);
    free(out);

    char *got = slurp(path);
    EXPECT_STR_EQ(got, "alpha\nbeta\n");
    free(got);

    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_write_creates_parent_dirs(void)
{
    char *dir = mk_tmpdir();
    char *path = xasprintf("%s/sub/deeper/file.txt", dir);

    char *out = call_write(path, "content\\n");
    EXPECT(strstr(out, "created ") != NULL);
    free(out);

    char *got = slurp(path);
    EXPECT_STR_EQ(got, "content\n");
    free(got);

    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_write_overwrites(void)
{
    char *dir = mk_tmpdir();
    char *path = xasprintf("%s/file.txt", dir);

    char *out = call_write(path, "first\\n");
    free(out);

    out = call_write(path, "second\\n");
    EXPECT(strstr(out, "-first") != NULL);
    EXPECT(strstr(out, "+second") != NULL);
    free(out);

    char *got = slurp(path);
    EXPECT_STR_EQ(got, "second\n");
    free(got);

    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_write_preserves_mode(void)
{
    char *dir = mk_tmpdir();
    char *path = xasprintf("%s/script.sh", dir);

    char *out = call_write(path, "echo hi\\n");
    free(out);
    chmod(path, 0750);

    out = call_write(path, "echo bye\\n");
    free(out);

    struct stat st;
    EXPECT(stat(path, &st) == 0);
    EXPECT((st.st_mode & 0777) == 0750);

    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_write_preserves_setuid(void)
{
    /* setuid/setgid/sticky must round-trip through a write. The fix
     * does two things: mask with 07777 (not 0777) when capturing the
     * existing mode, and apply fchmod *after* write_all (Linux clears
     * S_ISUID/S_ISGID on write by an unprivileged user). */
    char *dir = mk_tmpdir();
    char *path = xasprintf("%s/helper", dir);

    char *out = call_write(path, "old\\n");
    free(out);
    EXPECT(chmod(path, 04755) == 0); /* setuid + rwxr-xr-x */

    out = call_write(path, "new\\n");
    free(out);

    struct stat st;
    EXPECT(stat(path, &st) == 0);
    EXPECT((st.st_mode & 07777) == 04755);

    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_write_unchanged_yields_empty_diff(void)
{
    char *dir = mk_tmpdir();
    char *path = xasprintf("%s/file.txt", dir);

    char *out = call_write(path, "same\\n");
    free(out);

    /* Capture inode + mtime before the no-op write so we can confirm the
     * file was not replaced. With identical content, fs_write_with_diff
     * must skip the rename so hard links and inode-keyed metadata stay
     * intact. */
    struct stat before, after;
    EXPECT(stat(path, &before) == 0);

    out = call_write(path, "same\\n");
    EXPECT_STR_EQ(out, "");
    free(out);

    EXPECT(stat(path, &after) == 0);
    EXPECT(before.st_ino == after.st_ino);

    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_write_refuses_fifo(void)
{
    /* If we treated a FIFO as a regular file, slurp_file would block
     * forever waiting for a writer. The tool must refuse upfront with
     * a clear error rather than hang the agent. */
    char *dir = mk_tmpdir();
    char *path = xasprintf("%s/pipe", dir);
    EXPECT(mkfifo(path, 0644) == 0);

    char *out = call_write(path, "x\\n");
    EXPECT(strstr(out, "not a regular file") != NULL);
    free(out);

    /* The FIFO should still be a FIFO — not replaced by a regular file. */
    struct stat st;
    EXPECT(stat(path, &st) == 0);
    EXPECT(S_ISFIFO(st.st_mode));

    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_write_through_dangling_symlink(void)
{
    /* `link -> real` where `real` doesn't yet exist. Writing to the
     * link should create `real` while leaving the link intact — matches
     * the documented "write creates missing files" contract. */
    char *dir = mk_tmpdir();
    char *real = xasprintf("%s/real.txt", dir);
    char *link = xasprintf("%s/link.txt", dir);

    EXPECT(symlink(real, link) == 0); /* dangling on purpose */

    char *out = call_write(link, "hello\\n");
    /* Dangling-link target didn't exist, so this counts as a new-file
     * create — short confirmation, no diff. */
    EXPECT(strstr(out, "created ") != NULL);
    free(out);

    struct stat lst;
    EXPECT(lstat(link, &lst) == 0);
    EXPECT(S_ISLNK(lst.st_mode));

    char *got = slurp(real);
    EXPECT_STR_EQ(got, "hello\n");
    free(got);

    rm_rf(dir);
    free(real);
    free(link);
    free(dir);
}

static void test_write_empty_new_file(void)
{
    /* Creating an empty file is a valid request. The file must land on
     * disk *and* the tool must return an unambiguous success message so
     * the model can tell the difference from a no-op. */
    char *dir = mk_tmpdir();
    char *path = xasprintf("%s/empty.txt", dir);

    char *out = call_write(path, "");
    EXPECT(strstr(out, "created ") != NULL);
    EXPECT(strstr(out, "(empty)") != NULL);
    free(out);

    struct stat st;
    EXPECT(stat(path, &st) == 0);
    EXPECT(st.st_size == 0);

    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_write_through_symlink(void)
{
    char *dir = mk_tmpdir();
    char *real = xasprintf("%s/real.txt", dir);
    char *link = xasprintf("%s/link.txt", dir);

    char *out = call_write(real, "first\\n");
    free(out);
    EXPECT(symlink(real, link) == 0);

    /* Write through the symlink — the link must be preserved and the
     * underlying file's contents must change. */
    out = call_write(link, "second\\n");
    EXPECT(strstr(out, "+second") != NULL);
    free(out);

    struct stat lst;
    EXPECT(lstat(link, &lst) == 0);
    EXPECT(S_ISLNK(lst.st_mode));

    char *got = slurp(real);
    EXPECT_STR_EQ(got, "second\n");
    free(got);

    rm_rf(dir);
    free(real);
    free(link);
    free(dir);
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
    test_write_through_symlink();
    test_write_through_dangling_symlink();
    test_write_refuses_fifo();
    T_REPORT();
}
