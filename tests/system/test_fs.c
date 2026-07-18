/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "harness.h"
#include "util.h"
#include "system/fs.h"
#include "system/path.h"

/* Fresh scratch directory; caller frees the returned path. Test-created
 * contents are cleaned per-test with plain unlink/rmdir — the trees here
 * are tiny and explicit — but the root is harness-owned: t_tempdir()
 * removes it at exit, along with whatever a failing test leaves. */
static char *scratch_dir(void)
{
    return xstrdup(t_tempdir());
}

static void touch(const char *path, mode_t mode)
{
    int fd = open(path, O_CREAT | O_WRONLY, mode);
    EXPECT(fd >= 0);
    if (fd >= 0)
        close(fd);
}

/* ---------- fs_which ---------- */

static void test_which_finds_sh(void)
{
    /* `sh` exists on every supported platform; the result must be an
     * absolute path that is itself executable. */
    char *p = fs_which("sh");
    EXPECT(p != NULL);
    EXPECT(p && p[0] == '/');
    EXPECT(p && access(p, X_OK) == 0);
    free(p);
}

static void test_which_missing_is_null(void)
{
    char *p = fs_which("hax-definitely-not-a-real-binary");
    EXPECT(p == NULL);
    free(p);
}

static void test_which_slash_passes_through(void)
{
    char *p = fs_which("/bin/sh");
    EXPECT(p != NULL);
    EXPECT(p && strcmp(p, "/bin/sh") == 0);
    free(p);
}

static void test_which_slash_nonexecutable_is_null(void)
{
    char *p = fs_which("/dev/null/nope");
    EXPECT(p == NULL);
    free(p);
}

static void test_which_empty_and_null(void)
{
    EXPECT(fs_which("") == NULL);
    EXPECT(fs_which(NULL) == NULL);
}

static void test_which_skips_relative_path_entries(void)
{
    /* A PATH of only relative/empty entries must not resolve anything —
     * picking a binary out of cwd is the footgun fs_which refuses. */
    char *saved = xstrdup(getenv("PATH"));
    setenv("PATH", ".:relative/dir:", 1);
    char *p = fs_which("sh");
    EXPECT(p == NULL);
    free(p);
    setenv("PATH", saved, 1);
    free(saved);
}

static void test_which_resolves_in_later_entry(void)
{
    /* Earlier entries that don't exist or don't hold the name must not
     * stop the search. */
    char *dir = scratch_dir();
    char *exe = path_join(dir, "hax-test-tool");
    touch(exe, 0755);
    char *saved = xstrdup(getenv("PATH"));
    char *pathvar = xasprintf("/nonexistent-hax-dir:%s", dir);
    setenv("PATH", pathvar, 1);

    char *p = fs_which("hax-test-tool");
    EXPECT(p != NULL);
    EXPECT(p && strcmp(p, exe) == 0);

    setenv("PATH", saved, 1);
    free(p);
    free(pathvar);
    free(saved);
    unlink(exe);
    free(exe);
    free(dir);
}

static void test_which_skips_directory_match(void)
{
    /* A *directory* named like the target passes access(X_OK) (the x
     * bit means searchable), but exec'ing it would fail on every call.
     * It must be skipped in favor of a real file in a later entry. */
    char *dir_a = scratch_dir();
    char *dir_b = scratch_dir();
    char *decoy = path_join(dir_a, "hax-test-tool");
    EXPECT(mkdir(decoy, 0755) == 0);
    char *exe = path_join(dir_b, "hax-test-tool");
    touch(exe, 0755);
    char *saved = xstrdup(getenv("PATH"));
    char *pathvar = xasprintf("%s:%s", dir_a, dir_b);
    setenv("PATH", pathvar, 1);

    char *p = fs_which("hax-test-tool");
    EXPECT(p != NULL);
    EXPECT(p && strcmp(p, exe) == 0);

    setenv("PATH", saved, 1);
    free(p);
    free(pathvar);
    free(saved);
    unlink(exe);
    rmdir(decoy);
    free(exe);
    free(decoy);
    free(dir_a);
    free(dir_b);
}

static void test_which_slash_directory_is_null(void)
{
    /* Slash form takes the same regular-file check: a searchable
     * directory (/tmp here) is not an executable. */
    char *p = fs_which("/tmp");
    EXPECT(p == NULL);
    free(p);
}

static void test_which_skips_non_executable(void)
{
    /* A matching name without the x bit is not a hit — same as which(1)
     * and shells' PATH search. */
    char *dir = scratch_dir();
    char *file = path_join(dir, "hax-test-tool");
    touch(file, 0644);
    char *saved = xstrdup(getenv("PATH"));
    setenv("PATH", dir, 1);

    char *p = fs_which("hax-test-tool");
    EXPECT(p == NULL);
    free(p);

    setenv("PATH", saved, 1);
    free(saved);
    unlink(file);
    free(file);
    free(dir);
}

/* ---------- fs_mkdir_p ---------- */

static void test_mkdir_p_creates_nested(void)
{
    char *dir = scratch_dir();
    char *deep = path_join(dir, "a/b/c");
    EXPECT(fs_mkdir_p(deep) == 0);
    struct stat st;
    EXPECT(stat(deep, &st) == 0 && S_ISDIR(st.st_mode));

    /* Idempotent: everything already exists. */
    EXPECT(fs_mkdir_p(deep) == 0);

    char *b = path_join(dir, "a/b");
    char *a = path_join(dir, "a");
    rmdir(deep);
    rmdir(b);
    rmdir(a);
    free(deep);
    free(b);
    free(a);
    free(dir);
}

static void test_mkdir_p_trailing_slashes(void)
{
    char *dir = scratch_dir();
    char *deep = path_join(dir, "x/y");
    char *slashed = xasprintf("%s//", deep);
    EXPECT(fs_mkdir_p(slashed) == 0);
    struct stat st;
    EXPECT(stat(deep, &st) == 0 && S_ISDIR(st.st_mode));

    char *x = path_join(dir, "x");
    rmdir(deep);
    rmdir(x);
    free(slashed);
    free(deep);
    free(x);
    free(dir);
}

static void test_mkdir_p_file_in_the_middle_fails(void)
{
    char *dir = scratch_dir();
    char *file = path_join(dir, "blocker");
    touch(file, 0644);
    char *deep = path_join(dir, "blocker/sub");
    errno = 0;
    EXPECT(fs_mkdir_p(deep) == -1);
    EXPECT(errno == ENOTDIR);

    unlink(file);
    free(deep);
    free(file);
    free(dir);
}

static void test_mkdir_p_final_component_is_file_fails(void)
{
    /* mkdir(2) reports EEXIST here; fs_mkdir_p must not report success
     * when what exists is not a directory — `mkdir -p` fails too. */
    char *dir = scratch_dir();
    char *file = path_join(dir, "taken");
    touch(file, 0644);
    errno = 0;
    EXPECT(fs_mkdir_p(file) == -1);
    EXPECT(errno == ENOTDIR);

    unlink(file);
    free(file);
    free(dir);
}

static void test_mkdir_p_null_and_empty(void)
{
    EXPECT(fs_mkdir_p(NULL) == 0);
    EXPECT(fs_mkdir_p("") == 0);
}

int main(void)
{
    test_which_finds_sh();
    test_which_missing_is_null();
    test_which_slash_passes_through();
    test_which_slash_nonexecutable_is_null();
    test_which_empty_and_null();
    test_which_skips_relative_path_entries();
    test_which_resolves_in_later_entry();
    test_which_skips_directory_match();
    test_which_slash_directory_is_null();
    test_which_skips_non_executable();

    test_mkdir_p_creates_nested();
    test_mkdir_p_trailing_slashes();
    test_mkdir_p_file_in_the_middle_fails();
    test_mkdir_p_final_component_is_file_fails();
    test_mkdir_p_null_and_empty();

    T_REPORT();
}
