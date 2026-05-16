/* SPDX-License-Identifier: MIT */
#include "harness.h"
#include "tools/bash_cd_strip.h"

static const char *CWD = "/Users/me/proj";
static const char *HOME = "/Users/me";

/* Helper: compare the stripped suffix of `cmd` against `want`. The
 * empty-string `want` asserts "no strip" (offset 0); a non-empty `want`
 * asserts the suffix equals `want` exactly. */
static void expect_strip(const char *cmd, const char *cwd, const char *home, const char *want)
{
    size_t off = bash_strip_cd_prefix(cmd, cwd, home);
    if (want == NULL || *want == '\0') {
        if (off != 0)
            FAIL("expected no strip for %s, got offset %zu (suffix=\"%s\")", cmd, off, cmd + off);
        return;
    }
    if (off == 0) {
        FAIL("expected strip for %s, got offset 0", cmd);
        return;
    }
    EXPECT_STR_EQ(cmd + off, want);
}

static void test_strip_absolute_path(void)
{
    expect_strip("cd /Users/me/proj && rg foo", CWD, HOME, "rg foo");
}

static void test_strip_absolute_path_trailing_slash(void)
{
    expect_strip("cd /Users/me/proj/ && rg foo", CWD, HOME, "rg foo");
}

static void test_strip_tilde_subpath(void)
{
    expect_strip("cd ~/proj && rg foo", CWD, HOME, "rg foo");
}

static void test_strip_bare_tilde_when_home_matches(void)
{
    expect_strip("cd ~ && pwd", HOME, HOME, "pwd");
}

static void test_strip_bare_tilde_when_home_doesnt_match(void)
{
    /* HOME != cwd: cd ~ would change directories — not a no-op. */
    expect_strip("cd ~ && pwd", CWD, HOME, "");
}

static void test_strip_home_variable(void)
{
    expect_strip("cd $HOME/proj && rg foo", CWD, HOME, "rg foo");
}

static void test_strip_home_braced(void)
{
    expect_strip("cd ${HOME}/proj && rg foo", CWD, HOME, "rg foo");
}

static void test_no_strip_pwd_variable(void)
{
    expect_strip("cd $PWD && rg foo", CWD, HOME, "");
    expect_strip("cd ${PWD} && rg foo", CWD, HOME, "");
    expect_strip("cd \"$PWD\" && rg foo", CWD, HOME, "");
}

static void test_strip_dot(void)
{
    expect_strip("cd . && rg foo", CWD, HOME, "rg foo");
    expect_strip("cd \".\" && rg foo", CWD, HOME, "rg foo");
    expect_strip("cd '.' && rg foo", CWD, HOME, "rg foo");
}

static void test_strip_double_quoted_absolute(void)
{
    expect_strip("cd \"/Users/me/proj\" && rg foo", CWD, HOME, "rg foo");
}

static void test_strip_double_quoted_home_var(void)
{
    expect_strip("cd \"$HOME/proj\" && rg foo", CWD, HOME, "rg foo");
}

static void test_strip_double_quoted_braced_home(void)
{
    expect_strip("cd \"${HOME}/proj\" && rg foo", CWD, HOME, "rg foo");
}

static void test_strip_single_quoted_literal(void)
{
    expect_strip("cd '/Users/me/proj' && rg foo", CWD, HOME, "rg foo");
}

static void test_single_quoted_tilde_is_literal(void)
{
    /* `'~/proj'` is literally the directory `~/proj`, not $HOME/proj.
     * Stripping here would change semantics — keep as-is. */
    expect_strip("cd '~/proj' && rg foo", CWD, HOME, "");
}

static void test_double_quoted_tilde_is_literal(void)
{
    /* Same story for double quotes — bash does not perform tilde
     * expansion inside `"..."`. */
    expect_strip("cd \"~/proj\" && rg foo", CWD, HOME, "");
}

static void test_no_strip_when_cd_target_differs(void)
{
    expect_strip("cd /elsewhere && rg foo", CWD, HOME, "");
    expect_strip("cd ~/other && rg foo", CWD, HOME, "");
    expect_strip("cd $HOME/other && rg foo", CWD, HOME, "");
}

static void test_no_strip_on_relative_path(void)
{
    expect_strip("cd proj && rg foo", "/Users/me", HOME, "");
    expect_strip("cd ../proj && rg foo", "/Users/me/other", HOME, "");
}

static void test_no_strip_when_no_double_amp(void)
{
    expect_strip("cd /Users/me/proj; rg foo", CWD, HOME, "");
    expect_strip("cd /Users/me/proj & rg foo", CWD, HOME, "");
    expect_strip("cd /Users/me/proj || rg foo", CWD, HOME, "");
    expect_strip("cd /Users/me/proj | rg foo", CWD, HOME, "");
}

static void test_no_strip_on_command_substitution(void)
{
    expect_strip("cd $(pwd) && rg foo", CWD, HOME, "");
    expect_strip("cd `pwd` && rg foo", CWD, HOME, "");
}

static void test_no_strip_on_glob_or_escape(void)
{
    expect_strip("cd /Users/me/pro* && rg foo", CWD, HOME, "");
    expect_strip("cd /Users/me/\\proj && rg foo", CWD, HOME, "");
}

static void test_no_strip_on_userhome_form(void)
{
    /* `~oleksandr/proj` needs getpwnam — we don't try. */
    expect_strip("cd ~oleksandr/proj && rg foo", CWD, HOME, "");
}

static void test_no_strip_on_homex_variable(void)
{
    /* $HOMEx is a different (typically unset) variable. */
    expect_strip("cd $HOMEx/proj && rg foo", CWD, HOME, "");
}

static void test_no_strip_when_no_cd(void)
{
    expect_strip("ls && rg foo", CWD, HOME, "");
    expect_strip("rg foo", CWD, HOME, "");
}

static void test_no_strip_when_cd_alone(void)
{
    expect_strip("cd && rg foo", CWD, HOME, "");
}

static void test_no_strip_when_empty_suffix(void)
{
    expect_strip("cd /Users/me/proj && ", CWD, HOME, "");
    expect_strip("cd /Users/me/proj &&", CWD, HOME, "");
}

static void test_leading_whitespace_tolerated(void)
{
    expect_strip("   cd /Users/me/proj && rg foo", CWD, HOME, "rg foo");
}

static void test_no_strip_when_home_unset(void)
{
    /* HOME=NULL means we can't expand ~ or $HOME safely. */
    expect_strip("cd ~/proj && rg foo", CWD, NULL, "");
    expect_strip("cd $HOME/proj && rg foo", CWD, NULL, "");
    /* But an absolute literal still works. */
    expect_strip("cd /Users/me/proj && rg foo", CWD, NULL, "rg foo");
}

static void test_no_strip_on_mixed_quoting(void)
{
    /* `"/a"foo` is bash word concatenation — too clever for us. */
    expect_strip("cd \"/Users/me/proj\"x && rg foo", CWD, HOME, "");
}

static void test_no_strip_on_backslash_in_double_quotes(void)
{
    expect_strip("cd \"/Users/me/\\proj\" && rg foo", CWD, HOME, "");
}

static void test_strip_cwd_is_root(void)
{
    expect_strip("cd / && ls", "/", HOME, "ls");
    expect_strip("cd \"/\" && ls", "/", HOME, "ls");
}

static void test_strip_tight_amp(void)
{
    /* `&&` with no surrounding spaces. */
    expect_strip("cd /Users/me/proj&&rg foo", CWD, HOME, "rg foo");
}

/* Unquoted $HOME with whitespace in the value: bash word-splits the
 * expansion, so the original `cd $HOME/proj` passes two args to `cd`
 * and fails. Stripping would silently turn a broken command into a
 * working one — exactly the semantics drift we promised to avoid. */
static void test_no_strip_unquoted_home_var_with_space(void)
{
    expect_strip("cd $HOME/proj && rg foo", "/tmp/a b/proj", "/tmp/a b", "");
    expect_strip("cd ${HOME}/proj && rg foo", "/tmp/a b/proj", "/tmp/a b", "");
    expect_strip("cd $HOME && pwd", "/tmp/a b", "/tmp/a b", "");
}

/* Quoted forms aren't subject to word splitting or pathname expansion,
 * so a space in $HOME is safe to substitute. */
static void test_strip_quoted_home_var_with_space(void)
{
    expect_strip("cd \"$HOME/proj\" && rg foo", "/tmp/a b/proj", "/tmp/a b", "rg foo");
    expect_strip("cd \"${HOME}/proj\" && rg foo", "/tmp/a b/proj", "/tmp/a b", "rg foo");
}

static void test_no_strip_unquoted_var_with_glob(void)
{
    /* `*` / `?` / `[` in the expanded value would be subject to
     * pathname expansion and could resolve to a different directory. */
    expect_strip("cd $HOME/proj && rg foo", "/tmp/a*/proj", "/tmp/a*", "");
    expect_strip("cd $HOME && ls", "/tmp/q?", "/tmp/q?", "");
}

/* Double quotes suppress word splitting and globbing, so spaces and
 * glob meta in the literal portion are safe to pass through. */
static void test_strip_double_quoted_absolute_with_space(void)
{
    expect_strip("cd \"/tmp/my proj\" && rg foo", "/tmp/my proj", HOME, "rg foo");
}

static void test_strip_double_quoted_home_var_with_space_in_suffix(void)
{
    expect_strip("cd \"$HOME/my proj\" && rg foo", "/Users/me/my proj", "/Users/me", "rg foo");
    expect_strip("cd \"${HOME}/my proj\" && rg foo", "/Users/me/my proj", "/Users/me", "rg foo");
}

static void test_no_strip_double_quoted_with_dollar(void)
{
    /* Literal `$` inside double quotes would still trigger parameter
     * expansion at runtime — bail rather than guess what it resolves to. */
    expect_strip("cd \"/tmp/$x\" && ls", "/tmp/foo", HOME, "");
}

static void test_no_strip_double_quoted_with_backtick(void)
{
    /* Backticks trigger command substitution inside double quotes. */
    expect_strip("cd \"/tmp/`cmd`\" && ls", "/tmp/foo", HOME, "");
}

static void test_no_strip_tilde_with_space_in_home(void)
{
    /* Tilde is the borderline case — POSIX exempts tilde from word
     * splitting but pathname expansion is per-word, and behavior
     * across shells is implementation-defined. Stay conservative. */
    expect_strip("cd ~/proj && rg foo", "/tmp/a b/proj", "/tmp/a b", "");
    expect_strip("cd ~ && pwd", "/tmp/a b", "/tmp/a b", "");
}

int main(void)
{
    test_strip_absolute_path();
    test_strip_absolute_path_trailing_slash();
    test_strip_tilde_subpath();
    test_strip_bare_tilde_when_home_matches();
    test_strip_bare_tilde_when_home_doesnt_match();
    test_strip_home_variable();
    test_strip_home_braced();
    test_no_strip_pwd_variable();
    test_strip_dot();
    test_strip_double_quoted_absolute();
    test_strip_double_quoted_home_var();
    test_strip_double_quoted_braced_home();
    test_strip_single_quoted_literal();
    test_single_quoted_tilde_is_literal();
    test_double_quoted_tilde_is_literal();
    test_no_strip_when_cd_target_differs();
    test_no_strip_on_relative_path();
    test_no_strip_when_no_double_amp();
    test_no_strip_on_command_substitution();
    test_no_strip_on_glob_or_escape();
    test_no_strip_on_userhome_form();
    test_no_strip_on_homex_variable();
    test_no_strip_when_no_cd();
    test_no_strip_when_cd_alone();
    test_no_strip_when_empty_suffix();
    test_leading_whitespace_tolerated();
    test_no_strip_when_home_unset();
    test_no_strip_on_mixed_quoting();
    test_no_strip_on_backslash_in_double_quotes();
    test_strip_cwd_is_root();
    test_strip_tight_amp();
    test_no_strip_unquoted_home_var_with_space();
    test_strip_quoted_home_var_with_space();
    test_no_strip_unquoted_var_with_glob();
    test_strip_double_quoted_absolute_with_space();
    test_strip_double_quoted_home_var_with_space_in_suffix();
    test_no_strip_double_quoted_with_dollar();
    test_no_strip_double_quoted_with_backtick();
    test_no_strip_tilde_with_space_in_home();
    T_REPORT();
}
