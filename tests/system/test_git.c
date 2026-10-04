/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
/* The wait macros are provided by <sys/wait.h> per POSIX; glibc also leaks
 * them through <stdlib.h>, so the include cleaner cannot attribute them. */
#include <sys/wait.h> // IWYU pragma: keep

#include "buf.h"
#include "harness.h"
#include "xalloc.h"
#include "system/fs.h"
#include "system/git.h"
#include "system/spawn.h"

static int git_available(void)
{
    char *path = fs_which("git");
    int found = path != NULL;
    free(path);
    return found;
}

static void run_quiet(const char *command)
{
    char *silenced = xasprintf("%s >/dev/null 2>&1", command);
    int status = spawn_shell_wait(silenced);
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    free(silenced);
}

/* A working directory that git cannot mistake for part of an enclosing repository: the ceiling
 * stops the upward search, so the probe sees exactly what this test built. */
static void enter_tempdir(void)
{
    char *dir = t_tempdir();
    EXPECT(chdir(dir) == 0);
    setenv("GIT_CEILING_DIRECTORIES", dir, 1);
    setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1);
    setenv("GIT_CONFIG_SYSTEM", "/dev/null", 1);
    setenv("GIT_AUTHOR_NAME", "hax test", 1);
    setenv("GIT_AUTHOR_EMAIL", "test@example.com", 1);
    setenv("GIT_COMMITTER_NAME", "hax test", 1);
    setenv("GIT_COMMITTER_EMAIL", "test@example.com", 1);
}

static void init_repo(void)
{
    run_quiet("git init -q");
    /* Not `git init -b`: older git rejects the flag, and the branch name must be predictable. */
    run_quiet("git symbolic-ref HEAD refs/heads/topic");
}

static void test_outside_repository(void)
{
    if (!git_available())
        T_SKIP("git not installed");
    enter_tempdir();

    struct git_state state;
    git_state_probe(&state);
    EXPECT(state.branch == NULL);
    EXPECT(state.commit == NULL);
    EXPECT(state.subject == NULL);
    git_state_free(&state);
}

static void test_commit_is_described(void)
{
    if (!git_available())
        T_SKIP("git not installed");
    enter_tempdir();
    init_repo();
    run_quiet("echo hello > file.txt");
    run_quiet("git add file.txt");
    run_quiet("git commit -q -m 'Add the first file' -m 'Body text ignored'");

    struct git_state state;
    git_state_probe(&state);
    EXPECT_STR_EQ(state.branch, "topic");
    EXPECT_STR_EQ(state.subject, "Add the first file");
    EXPECT(state.commit != NULL);
    if (state.commit)
        EXPECT(strlen(state.commit) >= 7 && strchr(state.commit, '\n') == NULL);
    git_state_free(&state);
}

static void test_unborn_branch_has_no_commit(void)
{
    if (!git_available())
        T_SKIP("git not installed");
    enter_tempdir();
    init_repo();

    struct git_state state;
    git_state_probe(&state);
    EXPECT_STR_EQ(state.branch, "topic");
    EXPECT(state.commit == NULL);
    EXPECT(state.subject == NULL);
    git_state_free(&state);
}

static void test_detached_head_has_no_branch(void)
{
    if (!git_available())
        T_SKIP("git not installed");
    enter_tempdir();
    init_repo();
    run_quiet("echo hello > file.txt");
    run_quiet("git add file.txt");
    run_quiet("git commit -q -m 'Add the first file'");
    run_quiet("git checkout -q --detach HEAD");

    struct git_state state;
    git_state_probe(&state);
    EXPECT(state.branch == NULL);
    EXPECT_STR_EQ(state.subject, "Add the first file");
    git_state_free(&state);
}

/* A stub git on PATH that only records that it ran, by creating `marker`. Returns the saved PATH
 * for t_path_restore. */
static char *prepend_recording_git(const char *marker)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/git", dir);
    FILE *script = fopen(path, "w");
    EXPECT(script != NULL);
    if (script) {
        fprintf(script, "#!/bin/sh\n: > '%s'\n", marker);
        fclose(script);
    }
    EXPECT(chmod(path, 0755) == 0);
    free(path);
    return t_path_prepend(dir);
}

static void test_probe_runs_git_only_where_a_repository_may_be(void)
{
    enter_tempdir();
    char *marker = xasprintf("%s/ran", t_tempdir());
    char *saved_path = prepend_recording_git(marker);

    struct git_state state;
    git_state_probe(&state);
    EXPECT(access(marker, F_OK) != 0);
    git_state_free(&state);

    /* GIT_DIR may name a repository anywhere. */
    setenv("GIT_DIR", "/nonexistent", 1);
    git_state_probe(&state);
    unsetenv("GIT_DIR");
    EXPECT(access(marker, F_OK) == 0);
    git_state_free(&state);

    t_path_restore(saved_path);
    free(marker);
}

static void test_worktree_root_found_from_subdirectory(void)
{
    char *root = t_tempdir();
    char *marker = xasprintf("%s/.git", root);
    char *nested = xasprintf("%s/a/b", root);
    EXPECT(fs_mkdir_p(marker) == 0);
    EXPECT(fs_mkdir_p(nested) == 0);

    char *found = git_find_worktree_root(nested);
    EXPECT(found != NULL);
    if (found)
        EXPECT_STR_EQ(found, root);
    free(found);
    free(nested);
    free(marker);
}

static void test_worktree_root_marker_may_be_a_file(void)
{
    /* A linked worktree's .git is a file pointing at the main repository. */
    char *root = t_tempdir();
    char *marker = xasprintf("%s/.git", root);
    FILE *file = fopen(marker, "w");
    EXPECT(file != NULL);
    if (file)
        fclose(file);

    char *found = git_find_worktree_root(root);
    EXPECT(found != NULL);
    if (found)
        EXPECT_STR_EQ(found, root);
    free(found);
    free(marker);
}

static void test_worktree_root_marker_symlink_is_not_followed(void)
{
    char *root = t_tempdir();
    char *marker = xasprintf("%s/.git", root);
    EXPECT(symlink("/nonexistent/hax-test-git-dir", marker) == 0);

    char *found = git_find_worktree_root(root);
    EXPECT(found != NULL);
    if (found)
        EXPECT_STR_EQ(found, root);
    free(found);
    free(marker);
}

static void test_worktree_root_found_from_deep_subdirectory(void)
{
    char *root = t_tempdir();
    char *marker = xasprintf("%s/.git", root);
    EXPECT(fs_mkdir_p(marker) == 0);
    struct buf nested;
    buf_init(&nested);
    buf_append_str(&nested, root);
    for (int depth = 0; depth < 100; depth++)
        buf_append_str(&nested, "/d");
    EXPECT(fs_mkdir_p(nested.data) == 0);

    char *found = git_find_worktree_root(nested.data);
    EXPECT(found != NULL);
    if (found)
        EXPECT_STR_EQ(found, root);
    free(found);
    buf_free(&nested);
    free(marker);
}

static void test_worktree_root_absent_outside_repository(void)
{
    EXPECT(git_find_worktree_root(t_tempdir()) == NULL);
}

int main(void)
{
    test_worktree_root_found_from_subdirectory();
    test_worktree_root_marker_may_be_a_file();
    test_worktree_root_marker_symlink_is_not_followed();
    test_worktree_root_found_from_deep_subdirectory();
    test_worktree_root_absent_outside_repository();
    test_probe_runs_git_only_where_a_repository_may_be();
    test_outside_repository();
    test_commit_is_described();
    test_unborn_branch_has_no_commit();
    test_detached_head_has_no_branch();
    T_REPORT();
}
