/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "util.h"
#include "system/path.h"

/* ---------- path_join ---------- */

static void test_path_join_simple(void)
{
    char *out = path_join("/tmp", "foo");
    EXPECT_STR_EQ(out, "/tmp/foo");
    free(out);
}

static void test_path_join_strips_trailing_slash(void)
{
    /* Original motivating bug: macOS $TMPDIR ends in '/' and naive
     * concat produces "//hax-bash-XXXXXX". */
    char *out = path_join("/var/folders/abc/T/", "hax-bash-XXXXXX");
    EXPECT_STR_EQ(out, "/var/folders/abc/T/hax-bash-XXXXXX");
    free(out);
}

static void test_path_join_strips_multiple_trailing_slashes(void)
{
    char *out = path_join("/tmp///", "foo");
    EXPECT_STR_EQ(out, "/tmp/foo");
    free(out);
}

static void test_path_join_strips_leading_slash_on_rel(void)
{
    /* `rel` arriving with a leading '/' (rare, but defensive) shouldn't
     * produce "//" in the join either. */
    char *out = path_join("/tmp", "/foo");
    EXPECT_STR_EQ(out, "/tmp/foo");
    free(out);
}

static void test_path_join_root_base(void)
{
    /* base=="/" must not be stripped to empty — joining with "etc"
     * needs to give "/etc", not "etc" or "//etc". */
    char *out = path_join("/", "etc");
    EXPECT_STR_EQ(out, "/etc");
    free(out);
}

static void test_path_join_root_base_with_leading_slash_rel(void)
{
    char *out = path_join("/", "/etc");
    EXPECT_STR_EQ(out, "/etc");
    free(out);
}

static void test_path_join_relative_base(void)
{
    char *out = path_join("subdir", "file.txt");
    EXPECT_STR_EQ(out, "subdir/file.txt");
    free(out);
}

static void test_path_join_dot_base(void)
{
    char *out = path_join(".", "file.txt");
    EXPECT_STR_EQ(out, "./file.txt");
    free(out);
}

static void test_path_join_empty_base(void)
{
    /* No current call site passes an empty base, but pin the behavior
     * so a future regression is loud rather than silent. blen==0 hits
     * neither the trailing-slash strip (guarded by blen>1) nor the
     * root special-case, so it falls through to "%.*s/%s" with .*==0,
     * yielding "/rel". */
    char *out = path_join("", "foo");
    EXPECT_STR_EQ(out, "/foo");
    free(out);
}

static void test_path_join_empty_rel(void)
{
    /* Symmetric: pin the empty-rel behavior. */
    char *out = path_join("/tmp", "");
    EXPECT_STR_EQ(out, "/tmp/");
    free(out);
}

/* ---------- expand_home ---------- */

static void test_expand_home_null(void)
{
    EXPECT(expand_home(NULL) == NULL);
}

static void test_expand_home_no_tilde(void)
{
    setenv("HOME", "/tmp/fake", 1);
    char *p = expand_home("/absolute/path");
    EXPECT_STR_EQ(p, "/absolute/path");
    free(p);
}

static void test_expand_home_tilde_only(void)
{
    setenv("HOME", "/tmp/fake", 1);
    char *p = expand_home("~");
    EXPECT_STR_EQ(p, "/tmp/fake");
    free(p);
}

static void test_expand_home_tilde_slash(void)
{
    setenv("HOME", "/tmp/fake", 1);
    char *p = expand_home("~/sub/file");
    EXPECT_STR_EQ(p, "/tmp/fake/sub/file");
    free(p);
}

static void test_expand_home_no_home_env(void)
{
    unsetenv("HOME");
    char *p = expand_home("~/foo");
    EXPECT_STR_EQ(p, "~/foo");
    free(p);
}

static void test_expand_home_empty_home_env(void)
{
    /* HOME set but empty must not produce a "/foo"-style path. Same fallback
     * as unset HOME: leave the tilde in place so the caller's open() error
     * tells the user the path is wrong rather than silently rooting at /. */
    setenv("HOME", "", 1);
    char *p = expand_home("~/foo");
    EXPECT_STR_EQ(p, "~/foo");
    free(p);
}

static void test_expand_home_tilde_user_left_alone(void)
{
    /* `~user/...` would need getpwnam — out of scope. Pass through unchanged
     * so the caller's open() surfaces the same "no such file" the user would
     * see in any non-shell context. */
    setenv("HOME", "/tmp/fake", 1);
    char *p = expand_home("~root/etc");
    EXPECT_STR_EQ(p, "~root/etc");
    free(p);
}

/* ---------- collapse_home ---------- */

static void test_collapse_home_null(void)
{
    EXPECT(collapse_home(NULL) == NULL);
}

static void test_collapse_home_substitutes_prefix(void)
{
    setenv("HOME", "/Users/alice", 1);
    char *p = collapse_home("/Users/alice/source/hax");
    EXPECT_STR_EQ(p, "~/source/hax");
    free(p);
}

static void test_collapse_home_exact_match(void)
{
    setenv("HOME", "/Users/alice", 1);
    char *p = collapse_home("/Users/alice");
    EXPECT_STR_EQ(p, "~");
    free(p);
}

static void test_collapse_home_no_match_returns_input(void)
{
    setenv("HOME", "/Users/alice", 1);
    char *p = collapse_home("/etc/hosts");
    EXPECT_STR_EQ(p, "/etc/hosts");
    free(p);
}

static void test_collapse_home_partial_component_not_a_match(void)
{
    /* $HOME=/Users/alice must not match "/Users/alice2/..." — the substring
     * is a prefix at the byte level but not at the path-component level.
     * Without the boundary check, "alice2" would silently render as
     * "~2/..." which would mislead the user about where the file lives. */
    setenv("HOME", "/Users/alice", 1);
    char *p = collapse_home("/Users/alice2/x");
    EXPECT_STR_EQ(p, "/Users/alice2/x");
    free(p);
}

static void test_collapse_home_trailing_slash_in_home(void)
{
    /* $HOME with a trailing slash (rare, but possible if user-set) still
     * matches a path that doesn't have one. */
    setenv("HOME", "/Users/alice/", 1);
    char *p = collapse_home("/Users/alice/foo");
    EXPECT_STR_EQ(p, "~/foo");
    free(p);
}

static void test_collapse_home_no_home_env(void)
{
    unsetenv("HOME");
    char *p = collapse_home("/Users/alice/foo");
    EXPECT_STR_EQ(p, "/Users/alice/foo");
    free(p);
}

static void test_collapse_home_root_home(void)
{
    /* HOME=="/" is degenerate but possible (containers, daemons). The
     * trailing-slash strip is guarded by hlen > 1, so root is preserved
     * and any absolute path collapses to `~/...`. */
    setenv("HOME", "/", 1);
    char *p = collapse_home("/etc/hosts");
    EXPECT_STR_EQ(p, "~/etc/hosts");
    free(p);
}

int main(void)
{
    test_path_join_simple();
    test_path_join_strips_trailing_slash();
    test_path_join_strips_multiple_trailing_slashes();
    test_path_join_strips_leading_slash_on_rel();
    test_path_join_root_base();
    test_path_join_root_base_with_leading_slash_rel();
    test_path_join_relative_base();
    test_path_join_dot_base();
    test_path_join_empty_base();
    test_path_join_empty_rel();

    test_expand_home_null();
    test_expand_home_no_tilde();
    test_expand_home_tilde_only();
    test_expand_home_tilde_slash();
    test_expand_home_no_home_env();
    test_expand_home_empty_home_env();
    test_expand_home_tilde_user_left_alone();

    test_collapse_home_null();
    test_collapse_home_substitutes_prefix();
    test_collapse_home_exact_match();
    test_collapse_home_no_match_returns_input();
    test_collapse_home_partial_component_not_a_match();
    test_collapse_home_trailing_slash_in_home();
    test_collapse_home_no_home_env();
    test_collapse_home_root_home();

    T_REPORT();
}
