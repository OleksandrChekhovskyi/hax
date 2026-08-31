/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* The wait macros are provided by <sys/wait.h> per POSIX; glibc also leaks
 * them through <stdlib.h>, so the include cleaner cannot attribute them. */
#include <sys/wait.h> // IWYU pragma: keep

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

int main(void)
{
    test_outside_repository();
    test_commit_is_described();
    test_unborn_branch_has_no_commit();
    test_detached_head_has_no_branch();
    T_REPORT();
}
