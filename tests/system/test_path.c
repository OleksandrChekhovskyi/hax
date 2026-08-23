/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "harness.h"
#include "system/path.h"

static void test_path_join_simple(void)
{
    char *joined = path_join("/tmp", "foo");
    EXPECT_STR_EQ(joined, "/tmp/foo");
    free(joined);
}

static void test_path_join_strips_trailing_slash(void)
{
    char *joined = path_join("/var/folders/abc/T/", "hax-bash-XXXXXX");
    EXPECT_STR_EQ(joined, "/var/folders/abc/T/hax-bash-XXXXXX");
    free(joined);
}

static void test_path_join_strips_multiple_trailing_slashes(void)
{
    char *joined = path_join("/tmp///", "foo");
    EXPECT_STR_EQ(joined, "/tmp/foo");
    free(joined);
}

static void test_path_join_strips_leading_slash_from_suffix(void)
{
    char *joined = path_join("/tmp", "/foo");
    EXPECT_STR_EQ(joined, "/tmp/foo");
    free(joined);
}

static void test_path_join_root_base(void)
{
    char *joined = path_join("/", "etc");
    EXPECT_STR_EQ(joined, "/etc");
    free(joined);
}

static void test_path_join_root_with_leading_slash_suffix(void)
{
    char *joined = path_join("/", "/etc");
    EXPECT_STR_EQ(joined, "/etc");
    free(joined);
}

static void test_path_join_relative_base(void)
{
    char *joined = path_join("subdir", "file.txt");
    EXPECT_STR_EQ(joined, "subdir/file.txt");
    free(joined);
}

static void test_path_join_dot_base(void)
{
    char *joined = path_join(".", "file.txt");
    EXPECT_STR_EQ(joined, "./file.txt");
    free(joined);
}

static void test_path_join_empty_base(void)
{
    char *joined = path_join("", "foo");
    EXPECT_STR_EQ(joined, "/foo");
    free(joined);
}

static void test_path_join_empty_suffix(void)
{
    char *joined = path_join("/tmp", "");
    EXPECT_STR_EQ(joined, "/tmp/");
    free(joined);
}

static void test_path_expand_home_null(void)
{
    EXPECT(path_expand_home(NULL) == NULL);
}

static void test_path_expand_home_copies_path_without_tilde(void)
{
    setenv("HOME", "/tmp/fake", 1);
    char *expanded = path_expand_home("/absolute/path");
    EXPECT_STR_EQ(expanded, "/absolute/path");
    free(expanded);
}

static void test_path_expand_home_bare_tilde(void)
{
    setenv("HOME", "/tmp/fake", 1);
    char *expanded = path_expand_home("~");
    EXPECT_STR_EQ(expanded, "/tmp/fake");
    free(expanded);
}

static void test_path_expand_home_tilde_prefix(void)
{
    setenv("HOME", "/tmp/fake", 1);
    char *expanded = path_expand_home("~/sub/file");
    EXPECT_STR_EQ(expanded, "/tmp/fake/sub/file");
    free(expanded);
}

static void test_path_expand_home_avoids_duplicate_separator(void)
{
    setenv("HOME", "/tmp/fake/", 1);
    char *expanded = path_expand_home("~/sub/file");
    EXPECT_STR_EQ(expanded, "/tmp/fake/sub/file");
    free(expanded);
}

static void test_path_expand_home_root(void)
{
    setenv("HOME", "/", 1);
    char *expanded = path_expand_home("~/etc/hosts");
    EXPECT_STR_EQ(expanded, "/etc/hosts");
    free(expanded);
}

static void test_path_expand_home_without_home(void)
{
    unsetenv("HOME");
    char *expanded = path_expand_home("~/foo");
    EXPECT_STR_EQ(expanded, "~/foo");
    free(expanded);
}

static void test_path_expand_home_with_empty_home(void)
{
    setenv("HOME", "", 1);
    char *expanded = path_expand_home("~/foo");
    EXPECT_STR_EQ(expanded, "~/foo");
    free(expanded);
}

static void test_path_expand_home_leaves_named_home_unchanged(void)
{
    setenv("HOME", "/tmp/fake", 1);
    char *expanded = path_expand_home("~root/etc");
    EXPECT_STR_EQ(expanded, "~root/etc");
    free(expanded);
}

static void test_path_collapse_home_null(void)
{
    EXPECT(path_collapse_home(NULL) == NULL);
}

static void test_path_collapse_home_prefix(void)
{
    setenv("HOME", "/Users/alice", 1);
    char *collapsed = path_collapse_home("/Users/alice/source/hax");
    EXPECT_STR_EQ(collapsed, "~/source/hax");
    free(collapsed);
}

static void test_path_collapse_home_exact_match(void)
{
    setenv("HOME", "/Users/alice", 1);
    char *collapsed = path_collapse_home("/Users/alice");
    EXPECT_STR_EQ(collapsed, "~");
    free(collapsed);
}

static void test_path_collapse_home_copies_unrelated_path(void)
{
    setenv("HOME", "/Users/alice", 1);
    char *collapsed = path_collapse_home("/etc/hosts");
    EXPECT_STR_EQ(collapsed, "/etc/hosts");
    free(collapsed);
}

static void test_path_collapse_home_requires_component_boundary(void)
{
    setenv("HOME", "/Users/alice", 1);
    char *collapsed = path_collapse_home("/Users/alice2/x");
    EXPECT_STR_EQ(collapsed, "/Users/alice2/x");
    free(collapsed);
}

static void test_path_collapse_home_ignores_trailing_home_slash(void)
{
    setenv("HOME", "/Users/alice/", 1);
    char *collapsed = path_collapse_home("/Users/alice/foo");
    EXPECT_STR_EQ(collapsed, "~/foo");
    free(collapsed);
}

static void test_path_collapse_home_without_home(void)
{
    unsetenv("HOME");
    char *collapsed = path_collapse_home("/Users/alice/foo");
    EXPECT_STR_EQ(collapsed, "/Users/alice/foo");
    free(collapsed);
}

static void test_path_collapse_home_with_empty_home(void)
{
    setenv("HOME", "", 1);
    char *collapsed = path_collapse_home("/Users/alice/foo");
    EXPECT_STR_EQ(collapsed, "/Users/alice/foo");
    free(collapsed);
}

static void test_path_collapse_root_home(void)
{
    setenv("HOME", "/", 1);
    char *collapsed = path_collapse_home("/etc/hosts");
    EXPECT_STR_EQ(collapsed, "~/etc/hosts");
    free(collapsed);
}

static void test_path_collapse_root_home_exact_match(void)
{
    setenv("HOME", "/", 1);
    char *collapsed = path_collapse_home("/");
    EXPECT_STR_EQ(collapsed, "~");
    free(collapsed);
}

static void test_path_relativize_descendant(void)
{
    char *relative = path_relativize("/home/u/proj/src/x.c", "/home/u/proj");
    EXPECT_STR_EQ(relative, "src/x.c");
    free(relative);
}

static void test_path_relativize_direct_child(void)
{
    char *relative = path_relativize("/home/u/proj/calc.py", "/home/u/proj");
    EXPECT_STR_EQ(relative, "calc.py");
    free(relative);
}

static void test_path_relativize_rejects_cwd(void)
{
    EXPECT(path_relativize("/home/u/proj", "/home/u/proj") == NULL);
}

static void test_path_relativize_rejects_unrelated_path(void)
{
    EXPECT(path_relativize("/etc/hosts", "/home/u/proj") == NULL);
}

static void test_path_relativize_requires_component_boundary(void)
{
    EXPECT(path_relativize("/home/u/proj2/x", "/home/u/proj") == NULL);
}

static void test_path_relativize_rejects_relative_path(void)
{
    EXPECT(path_relativize("src/x.c", "/home/u/proj") == NULL);
}

static void test_path_relativize_rejects_relative_cwd(void)
{
    EXPECT(path_relativize("/home/u/proj/x.c", "home/u/proj") == NULL);
}

static void test_path_relativize_ignores_trailing_cwd_slash(void)
{
    char *relative = path_relativize("/home/u/proj/x.c", "/home/u/proj/");
    EXPECT_STR_EQ(relative, "x.c");
    free(relative);
}

static void test_path_relativize_from_root(void)
{
    char *relative = path_relativize("/etc/hosts", "/");
    EXPECT_STR_EQ(relative, "etc/hosts");
    free(relative);
}

static void test_path_relativize_rejects_root_from_root(void)
{
    EXPECT(path_relativize("/", "/") == NULL);
}

static void test_path_relativize_rejects_null_inputs(void)
{
    EXPECT(path_relativize(NULL, "/home") == NULL);
    EXPECT(path_relativize("/home/x", NULL) == NULL);
}

static void test_path_relativize_rejects_escaping_parent_component(void)
{
    EXPECT(path_relativize("/repo/../outside/file", "/repo") == NULL);
}

static void test_path_relativize_rejects_non_escaping_parent_component(void)
{
    EXPECT(path_relativize("/repo/a/../b/file", "/repo") == NULL);
}

static void test_path_relativize_rejects_trailing_parent_component(void)
{
    EXPECT(path_relativize("/repo/sub/..", "/repo") == NULL);
}

static void test_path_relativize_accepts_dots_within_component(void)
{
    char *relative = path_relativize("/repo/a..b/file", "/repo");
    EXPECT_STR_EQ(relative, "a..b/file");
    free(relative);
}

static void test_path_normalize_windows_drive(void)
{
    char *normalized = path_normalize_windows("C:\\Users\\alice\\repo");
    EXPECT_STR_EQ(normalized, "/c/Users/alice/repo");
    free(normalized);
}

static void test_path_normalize_windows_unc(void)
{
    char *normalized = path_normalize_windows("\\\\server\\share\\repo");
    EXPECT_STR_EQ(normalized, "//server/share/repo");
    free(normalized);
}

static void test_path_normalize_windows_relative(void)
{
    char *normalized = path_normalize_windows("src\\main.c");
    EXPECT_STR_EQ(normalized, "src/main.c");
    free(normalized);
}

int main(void)
{
    test_path_join_simple();
    test_path_join_strips_trailing_slash();
    test_path_join_strips_multiple_trailing_slashes();
    test_path_join_strips_leading_slash_from_suffix();
    test_path_join_root_base();
    test_path_join_root_with_leading_slash_suffix();
    test_path_join_relative_base();
    test_path_join_dot_base();
    test_path_join_empty_base();
    test_path_join_empty_suffix();

    test_path_expand_home_null();
    test_path_expand_home_copies_path_without_tilde();
    test_path_expand_home_bare_tilde();
    test_path_expand_home_tilde_prefix();
    test_path_expand_home_avoids_duplicate_separator();
    test_path_expand_home_root();
    test_path_expand_home_without_home();
    test_path_expand_home_with_empty_home();
    test_path_expand_home_leaves_named_home_unchanged();

    test_path_collapse_home_null();
    test_path_collapse_home_prefix();
    test_path_collapse_home_exact_match();
    test_path_collapse_home_copies_unrelated_path();
    test_path_collapse_home_requires_component_boundary();
    test_path_collapse_home_ignores_trailing_home_slash();
    test_path_collapse_home_without_home();
    test_path_collapse_home_with_empty_home();
    test_path_collapse_root_home();
    test_path_collapse_root_home_exact_match();

    test_path_relativize_descendant();
    test_path_relativize_direct_child();
    test_path_relativize_rejects_cwd();
    test_path_relativize_rejects_unrelated_path();
    test_path_relativize_requires_component_boundary();
    test_path_relativize_rejects_relative_path();
    test_path_relativize_rejects_relative_cwd();
    test_path_relativize_ignores_trailing_cwd_slash();
    test_path_relativize_from_root();
    test_path_relativize_rejects_root_from_root();
    test_path_relativize_rejects_null_inputs();
    test_path_relativize_rejects_escaping_parent_component();
    test_path_relativize_rejects_non_escaping_parent_component();
    test_path_relativize_rejects_trailing_parent_component();
    test_path_relativize_accepts_dots_within_component();

    test_path_normalize_windows_drive();
    test_path_normalize_windows_unc();
    test_path_normalize_windows_relative();

    T_REPORT();
}
