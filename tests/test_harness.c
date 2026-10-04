/* SPDX-License-Identifier: MIT */
/* Forked children exit via exit(), not _exit(): the t_tempdir() cleanup under test is an
 * atexit handler. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "harness.h"

static int is_dir(const char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0 && S_ISDIR(sb.st_mode);
}

static pid_t fork_flushed(void)
{
    fflush(NULL); /* keep buffered output from duplicating into the child */
    pid_t pid = fork();
    EXPECT(pid >= 0);
    return pid;
}

static void test_child_exit_leaves_parent_dirs(void)
{
    char *dir = t_tempdir();
    pid_t pid = fork_flushed();
    if (pid == 0)
        exit(0); /* inherited cleanup must no-op: the parent owns dir */
    int st = 0;
    EXPECT(waitpid(pid, &st, 0) == pid);
    EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    EXPECT(is_dir(dir));
}

static void test_child_tempdir_cleaned_on_child_exit(void)
{
    /* Parent-owned dir created before the fork: the child inherits the
     * parent's bookkeeping and must still clean its own dir on exit —
     * while leaving this one alone. */
    char *parent_dir = t_tempdir();

    int fds[2];
    EXPECT(pipe(fds) == 0);
    pid_t pid = fork_flushed();
    if (pid == 0) {
        /* Snapshot the inherited path first: if t_tempdir() (wrongly)
         * freed the inherited entries, its own strdup could reuse the
         * storage and parent_dir would silently alias the child's dir —
         * so compare against the copy, not just stat the pointer. */
        char saved[256] = {0};
        snprintf(saved, sizeof(saved), "%s", parent_dir);
        char *dir = t_tempdir();
        int parent_ok = strcmp(parent_dir, saved) == 0 && strcmp(dir, saved) != 0 && is_dir(saved);
        ssize_t n = write(fds[1], dir, strlen(dir));
        exit(!parent_ok || n != (ssize_t)strlen(dir));
    }
    close(fds[1]);
    char child_dir[256] = {0};
    ssize_t n = read(fds[0], child_dir, sizeof(child_dir) - 1);
    close(fds[0]);
    int st = 0;
    EXPECT(waitpid(pid, &st, 0) == pid);
    EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    EXPECT(n > 0);
    struct stat sb;
    EXPECT(stat(child_dir, &sb) != 0); /* removed by the child's handler */
    EXPECT(is_dir(parent_dir));
}

static void test_child_cleans_unsearchable_tree(void)
{
    /* Tests may lock fixture dirs down to mode 0000 to provoke EACCES;
     * cleanup must restore permissions first or it silently reverts to
     * leaking, since nothing can traverse the locked dir. Nested
     * locked dirs pin the traversal order too: each dir must be
     * chmod'ed before cleanup descends into it. */
    int fds[2];
    EXPECT(pipe(fds) == 0);
    pid_t pid = fork_flushed();
    if (pid == 0) {
        char *dir = t_tempdir();
        char sub[256], inner[256], file[256];
        snprintf(sub, sizeof(sub), "%s/locked", dir);
        snprintf(inner, sizeof(inner), "%s/locked/inner", dir);
        snprintf(file, sizeof(file), "%s/locked/inner/f", dir);
        int ok = mkdir(sub, 0755) == 0 && mkdir(inner, 0755) == 0;
        int fd = open(file, O_CREAT | O_WRONLY, 0644);
        ok = ok && fd >= 0 && close(fd) == 0;
        /* Innermost first — it is unreachable once the parent locks. */
        ok = ok && chmod(inner, 0) == 0 && chmod(sub, 0) == 0;
        ssize_t n = write(fds[1], dir, strlen(dir));
        exit(!ok || n != (ssize_t)strlen(dir));
    }
    close(fds[1]);
    char child_dir[256] = {0};
    ssize_t n = read(fds[0], child_dir, sizeof(child_dir) - 1);
    close(fds[0]);
    int st = 0;
    EXPECT(waitpid(pid, &st, 0) == pid);
    EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    EXPECT(n > 0);
    struct stat sb;
    EXPECT(stat(child_dir, &sb) != 0); /* locked tree removed regardless */
}

static void test_cleanup_leaves_hard_linked_file_modes(void)
{
    /* A hard link inside a fixture shares its inode with the external
     * path. Cleanup must not touch file modes while removing the fixture,
     * or the external file's permissions get rewritten through the link. */
    char *outside = t_tempdir(); /* parent-owned; survives the child */
    char file[256];
    snprintf(file, sizeof(file), "%s/keep", outside);
    int fd = open(file, O_CREAT | O_WRONLY, 0600);
    EXPECT(fd >= 0 && close(fd) == 0);
    EXPECT(chmod(file, 0400) == 0);

    pid_t pid = fork_flushed();
    if (pid == 0) {
        char *dir = t_tempdir();
        char lnk[256];
        snprintf(lnk, sizeof(lnk), "%s/lnk", dir);
        exit(link(file, lnk) != 0);
    }
    int st = 0;
    EXPECT(waitpid(pid, &st, 0) == pid);
    EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    struct stat sb;
    EXPECT(stat(file, &sb) == 0);
    EXPECT((sb.st_mode & 0777) == 0400); /* untouched by child cleanup */
}

static void test_cleanup_does_not_follow_symlinks(void)
{
    char *outside = t_tempdir(); /* parent-owned; survives the child */
    char file[256];
    snprintf(file, sizeof(file), "%s/keep", outside);
    int fd = open(file, O_CREAT | O_WRONLY, 0600);
    EXPECT(fd >= 0 && close(fd) == 0);

    pid_t pid = fork_flushed();
    if (pid == 0) {
        char *dir = t_tempdir();
        char lnk[256];
        snprintf(lnk, sizeof(lnk), "%s/lnk", dir);
        exit(symlink(outside, lnk) != 0);
    }
    int st = 0;
    EXPECT(waitpid(pid, &st, 0) == pid);
    EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    struct stat sb;
    EXPECT(stat(file, &sb) == 0); /* the link went, not its target's contents */
}

static int path_is(const char *want)
{
    const char *path = getenv("PATH");
    return path && strcmp(path, want) == 0;
}

static void test_path_replace_round_trips_unset(void)
{
    char *original = t_path_replace("/one:/two");
    EXPECT(path_is("/one:/two"));

    char *before_unset = t_path_replace(NULL);
    EXPECT(getenv("PATH") == NULL);
    EXPECT_STR_EQ(before_unset, "/one:/two");

    /* Saved from an unset PATH: restore must unset again, not install "". */
    char *from_unset = t_path_replace("/stub");
    EXPECT(from_unset == NULL);
    EXPECT(path_is("/stub"));
    t_path_restore(from_unset);
    EXPECT(getenv("PATH") == NULL);

    t_path_restore(before_unset);
    EXPECT(path_is("/one:/two"));
    t_path_restore(original);
}

static void test_path_prepend_shadows_without_trailing_colon(void)
{
    char *original = t_path_replace("/one:/two");

    char *before = t_path_prepend("/stub");
    EXPECT(path_is("/stub:/one:/two"));
    EXPECT_STR_EQ(before, "/one:/two");
    t_path_restore(before);
    EXPECT(path_is("/one:/two"));

    /* Nothing to keep behind the stub: "/stub:" would also search the current directory. */
    char *before_unset = t_path_replace(NULL);
    char *from_unset = t_path_prepend("/stub");
    EXPECT(from_unset == NULL);
    EXPECT(path_is("/stub"));
    t_path_restore(from_unset);
    EXPECT(getenv("PATH") == NULL);
    t_path_restore(before_unset);

    char *before_empty = t_path_replace("");
    char *from_empty = t_path_prepend("/stub");
    EXPECT(path_is("/stub"));
    EXPECT_STR_EQ(from_empty, "");
    t_path_restore(from_empty);
    EXPECT(path_is(""));
    t_path_restore(before_empty);

    EXPECT(path_is("/one:/two"));
    t_path_restore(original);
}

int main(void)
{
    test_child_exit_leaves_parent_dirs();
    test_child_tempdir_cleaned_on_child_exit();
    test_child_cleans_unsearchable_tree();
    test_cleanup_leaves_hard_linked_file_modes();
    test_cleanup_does_not_follow_symlinks();
    test_path_replace_round_trips_unset();
    test_path_prepend_shadows_without_trailing_colon();
    T_REPORT();
}
