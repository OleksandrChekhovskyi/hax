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

static void touch_file(const char *path, mode_t mode)
{
    int fd = open(path, O_CREAT | O_WRONLY, mode);
    EXPECT(fd >= 0);
    if (fd >= 0)
        close(fd);
}

static char *replace_path_env(const char *value)
{
    const char *current = getenv("PATH");
    char *saved = current ? xstrdup(current) : NULL;
    if (value)
        setenv("PATH", value, 1);
    else
        unsetenv("PATH");
    return saved;
}

static void restore_path_env(char *saved)
{
    if (saved)
        setenv("PATH", saved, 1);
    else
        unsetenv("PATH");
    free(saved);
}

static void test_which_finds_sh(void)
{
    /* `sh` is present on every supported platform. */
    char *path = fs_which("sh");
    EXPECT(path != NULL);
    EXPECT(path && path[0] == '/');
    EXPECT(path && access(path, X_OK) == 0);
    free(path);
}

static void test_which_missing_is_null(void)
{
    char *path = fs_which("hax-definitely-not-a-real-binary");
    EXPECT(path == NULL);
    free(path);
}

static void test_which_slash_passes_through(void)
{
    char *path = fs_which("/bin/sh");
    EXPECT(path != NULL);
    EXPECT(path && strcmp(path, "/bin/sh") == 0);
    free(path);
}

static void test_which_slash_nonexecutable_is_null(void)
{
    char *path = fs_which("/dev/null/nope");
    EXPECT(path == NULL);
    free(path);
}

static void test_which_empty_null_and_unset_path(void)
{
    EXPECT(fs_which("") == NULL);
    EXPECT(fs_which(NULL) == NULL);

    char *saved_path = replace_path_env(NULL);
    EXPECT(fs_which("sh") == NULL);
    restore_path_env(saved_path);
}

static void test_which_skips_relative_path_entries(void)
{
    char *saved_path = replace_path_env(".:relative/dir:");
    char *path = fs_which("sh");
    EXPECT(path == NULL);
    free(path);
    restore_path_env(saved_path);
}

static void test_which_resolves_in_later_entry(void)
{
    const char *dir = t_tempdir();
    char *executable_path = path_join(dir, "hax-test-tool");
    touch_file(executable_path, 0755);
    char *path_env = xasprintf("/nonexistent-hax-dir:%s", dir);
    char *saved_path = replace_path_env(path_env);

    char *resolved_path = fs_which("hax-test-tool");
    EXPECT(resolved_path != NULL);
    EXPECT(resolved_path && strcmp(resolved_path, executable_path) == 0);

    restore_path_env(saved_path);
    free(resolved_path);
    free(path_env);
    free(executable_path);
}

static void test_which_skips_directory_match(void)
{
    /* access(X_OK) accepts searchable directories, so a later regular file must win. */
    const char *first_dir = t_tempdir();
    const char *second_dir = t_tempdir();
    char *directory_match = path_join(first_dir, "hax-test-tool");
    EXPECT(mkdir(directory_match, 0755) == 0);
    char *executable_path = path_join(second_dir, "hax-test-tool");
    touch_file(executable_path, 0755);
    char *path_env = xasprintf("%s:%s", first_dir, second_dir);
    char *saved_path = replace_path_env(path_env);

    char *resolved_path = fs_which("hax-test-tool");
    EXPECT(resolved_path != NULL);
    EXPECT(resolved_path && strcmp(resolved_path, executable_path) == 0);

    restore_path_env(saved_path);
    free(resolved_path);
    free(path_env);
    free(executable_path);
    free(directory_match);
}

static void test_which_slash_directory_is_null(void)
{
    char *path = fs_which("/tmp");
    EXPECT(path == NULL);
    free(path);
}

static void test_which_skips_non_executable(void)
{
    const char *dir = t_tempdir();
    char *file_path = path_join(dir, "hax-test-tool");
    touch_file(file_path, 0644);
    char *saved_path = replace_path_env(dir);

    char *resolved_path = fs_which("hax-test-tool");
    EXPECT(resolved_path == NULL);
    free(resolved_path);

    restore_path_env(saved_path);
    free(file_path);
}

static void test_resolve_link_target_regular_file(void)
{
    const char *dir = t_tempdir();
    char *path = path_join(dir, "file");
    touch_file(path, 0644);

    char *resolved_path = fs_resolve_link_target(path);
    EXPECT_STR_EQ(resolved_path, path);

    free(resolved_path);
    free(path);
}

static void test_resolve_link_target_missing_file(void)
{
    const char *dir = t_tempdir();
    char *path = path_join(dir, "missing");

    char *resolved_path = fs_resolve_link_target(path);
    EXPECT_STR_EQ(resolved_path, path);

    free(resolved_path);
    free(path);
}

static void test_resolve_link_target_relative_chain(void)
{
    const char *dir = t_tempdir();
    char *first_link = path_join(dir, "first");
    char *second_link = path_join(dir, "second");
    char *target_path = path_join(dir, "target");
    EXPECT(symlink("second", first_link) == 0);
    EXPECT(symlink("target", second_link) == 0);

    char *resolved_path = fs_resolve_link_target(first_link);
    EXPECT_STR_EQ(resolved_path, target_path);

    free(resolved_path);
    free(target_path);
    free(second_link);
    free(first_link);
}

static void test_resolve_link_target_long_target(void)
{
    const char *dir = t_tempdir();
    char *link_path = path_join(dir, "link");
    struct buf target;
    buf_init(&target);
    for (int i = 0; i < 30; i++)
        buf_append_str(&target, "missing/../");
    buf_append_str(&target, "target");
    EXPECT(symlink(target.data, link_path) == 0);

    char *expected_path = path_join(dir, target.data);
    char *resolved_path = fs_resolve_link_target(link_path);
    EXPECT_STR_EQ(resolved_path, expected_path);

    free(resolved_path);
    free(expected_path);
    buf_free(&target);
    free(link_path);
}

static void test_resolve_link_target_loop(void)
{
    const char *dir = t_tempdir();
    char *first_link = path_join(dir, "first");
    char *second_link = path_join(dir, "second");
    EXPECT(symlink("second", first_link) == 0);
    EXPECT(symlink("first", second_link) == 0);

    errno = 0;
    char *resolved_path = fs_resolve_link_target(first_link);
    EXPECT(resolved_path == NULL);
    EXPECT(errno == ELOOP);

    free(resolved_path);
    free(second_link);
    free(first_link);
}

static void test_mkdir_p_creates_nested(void)
{
    const char *dir = t_tempdir();
    char *path = path_join(dir, "a/b/c");
    EXPECT(fs_mkdir_p(path) == 0);
    struct stat st;
    EXPECT(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    EXPECT(fs_mkdir_p(path) == 0);
    free(path);
}

static void test_mkdir_p_trailing_slashes(void)
{
    const char *dir = t_tempdir();
    char *path = path_join(dir, "x/y");
    char *path_with_slashes = xasprintf("%s//", path);
    EXPECT(fs_mkdir_p(path_with_slashes) == 0);
    struct stat st;
    EXPECT(stat(path, &st) == 0 && S_ISDIR(st.st_mode));

    free(path_with_slashes);
    free(path);
}

static void test_mkdir_p_accepts_directory_symlink(void)
{
    const char *dir = t_tempdir();
    char *target_dir = path_join(dir, "target");
    char *link_path = path_join(dir, "link");
    char *nested_path = path_join(link_path, "nested");
    EXPECT(mkdir(target_dir, 0755) == 0);
    EXPECT(symlink(target_dir, link_path) == 0);

    EXPECT(fs_mkdir_p(nested_path) == 0);
    struct stat st;
    EXPECT(stat(nested_path, &st) == 0 && S_ISDIR(st.st_mode));

    free(nested_path);
    free(link_path);
    free(target_dir);
}

static void test_mkdir_p_file_in_the_middle_fails(void)
{
    const char *dir = t_tempdir();
    char *file_path = path_join(dir, "blocker");
    touch_file(file_path, 0644);
    char *nested_path = path_join(dir, "blocker/sub");

    errno = 0;
    EXPECT(fs_mkdir_p(nested_path) == -1);
    EXPECT(errno == ENOTDIR);

    free(nested_path);
    free(file_path);
}

static void test_mkdir_p_final_component_is_file_fails(void)
{
    const char *dir = t_tempdir();
    char *file_path = path_join(dir, "taken");
    touch_file(file_path, 0644);

    errno = 0;
    EXPECT(fs_mkdir_p(file_path) == -1);
    EXPECT(errno == ENOTDIR);

    free(file_path);
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
    test_which_empty_null_and_unset_path();
    test_which_skips_relative_path_entries();
    test_which_resolves_in_later_entry();
    test_which_skips_directory_match();
    test_which_slash_directory_is_null();
    test_which_skips_non_executable();

    test_resolve_link_target_regular_file();
    test_resolve_link_target_missing_file();
    test_resolve_link_target_relative_chain();
    test_resolve_link_target_long_target();
    test_resolve_link_target_loop();

    test_mkdir_p_creates_nested();
    test_mkdir_p_trailing_slashes();
    test_mkdir_p_accepts_directory_symlink();
    test_mkdir_p_file_in_the_middle_fails();
    test_mkdir_p_final_component_is_file_fails();
    test_mkdir_p_null_and_empty();

    T_REPORT();
}
