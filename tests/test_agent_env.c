/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "agent_env.h"
#include "config.h"
#include "harness.h"
#include "util.h"

/* Each test stages a fresh tmpdir tree (harness-cleaned at exit), chdirs into
 * it, runs agent_env_build_suffix(), then chdirs back. We also pin
 * HOME and XDG_CONFIG_HOME inside the sandbox so the developer's real
 * ~/.config/hax/AGENTS.md doesn't leak into test output, and clear the
 * HAX_NO_* knobs unless a test sets them deliberately. */

struct sandbox {
    char *root;     /* resolved t_tempdir; everything else lives under this */
    char *prev_cwd; /* cwd to restore on cleanup */
};

static void sandbox_init(struct sandbox *s)
{
    s->prev_cwd = getcwd(NULL, 0);
    /* t_tempdir hands back a canonical path, so this compares byte-for-byte against a later getcwd
     * when collapsing the home prefix. */
    s->root = xstrdup(t_tempdir());
    /* Point HOME and XDG_CONFIG_HOME at the sandbox so global AGENTS.md
     * lookups don't escape it. Tests that want a global file create it
     * under $HOME/.config/hax/AGENTS.md. */
    setenv("HOME", s->root, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
    unsetenv("HAX_NO_SKILLS");
    setenv("HAX_BASH_SHELL", CONFIG_VALUE_DEFAULT, 1);
    /* The subagents and tasks sections are static text present in every
     * suffix, which would smear across every assertion below; suppress them
     * by default and let the dedicated tests unset this deliberately. */
    setenv("HAX_NO_SUBAGENTS", "1", 1);
    setenv("HAX_NO_TASKS", "1", 1);
}

static void sandbox_free(struct sandbox *s)
{
    if (s->prev_cwd) {
        if (chdir(s->prev_cwd) != 0)
            FAIL("chdir(prev_cwd=%s): %s", s->prev_cwd, strerror(errno));
        free(s->prev_cwd);
    }
    free(s->root);
}

static void mkdirs(const char *path)
{
    char *p = xstrdup(path);
    for (char *c = p + 1; *c; c++) {
        if (*c == '/') {
            *c = '\0';
            mkdir(p, 0755);
            *c = '/';
        }
    }
    mkdir(p, 0755);
    free(p);
}

static void write_file_bytes(const char *path, const void *data, size_t len)
{
    char *dir = xstrdup(path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdirs(dir);
    }
    free(dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        FAIL("fopen(%s): %s", path, strerror(errno));
        return;
    }
    if (len && fwrite(data, 1, len, f) != len)
        FAIL("short write to %s", path);
    fclose(f);
}

static void write_file(const char *path, const char *content)
{
    write_file_bytes(path, content, strlen(content));
}

static int contains(const char *hay, const char *needle)
{
    return hay && strstr(hay, needle) != NULL;
}

/* ---------- sandbox-relative staging ---------- */

/* The helpers below take paths relative to the sandbox root and keep the
 * allocation and its free inside, so a test body reads as staging steps rather
 * than string plumbing. An empty or "." path names the root itself. Use
 * sandbox_path directly only where an absolute string outlives the call, such as
 * $PATH and $HOME assignments. */

static char *sandbox_path(struct sandbox *s, const char *rel)
{
    if (!rel || !*rel || strcmp(rel, ".") == 0)
        return xstrdup(s->root);
    return xasprintf("%s/%s", s->root, rel);
}

/* Create a directory and its parents. mkdirs ignores every mkdir error, so
 * confirm the result rather than letting a staging failure surface later as a
 * test that quietly checks nothing. */
static void sandbox_mkdir(struct sandbox *s, const char *rel)
{
    char *dir = sandbox_path(s, rel);
    mkdirs(dir);
    struct stat st;
    if (stat(dir, &st) != 0)
        FAIL("mkdir(%s): %s", dir, strerror(errno));
    else if (!S_ISDIR(st.st_mode))
        FAIL("mkdir(%s): not a directory", dir);
    free(dir);
}

static void sandbox_write(struct sandbox *s, const char *rel, const char *content)
{
    char *path = sandbox_path(s, rel);
    write_file(path, content);
    free(path);
}

static void sandbox_write_bytes(struct sandbox *s, const char *rel, const void *data, size_t len)
{
    char *path = sandbox_path(s, rel);
    write_file_bytes(path, data, len);
    free(path);
}

/* Stage a fake executable `name` in sandbox-relative directory `rel`, creating
 * it. Content is irrelevant — agent_env.c only checks access(X_OK). */
static void sandbox_stage_command(struct sandbox *s, const char *rel, const char *name)
{
    sandbox_mkdir(s, rel);
    char *dir = sandbox_path(s, rel);
    char *path = xasprintf("%s/%s", dir, name);
    free(dir);
    write_file(path, "#!/bin/sh\n");
    if (chmod(path, 0755) != 0)
        FAIL("chmod(%s): %s", path, strerror(errno));
    free(path);
}

static int sandbox_chdir(struct sandbox *s, const char *rel)
{
    char *dir = sandbox_path(s, rel);
    int ok = chdir(dir) == 0;
    if (!ok)
        FAIL("chdir(%s): %s", dir, strerror(errno));
    free(dir);
    return ok;
}

/* chdir into a sandbox-relative directory, or record the failure and leave the
 * test. Returning silently would skip every assertion below while the suite
 * still reported the test as passing. */
#define SANDBOX_CHDIR(s, rel)                                                                      \
    do {                                                                                           \
        if (!sandbox_chdir((s), (rel))) {                                                          \
            sandbox_free((s));                                                                     \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* Replace $PATH with a single directory, returning the previous value (NULL if
 * it was unset) for env_path_restore. Command probing must see the sandbox
 * only, or real /usr/bin tools would drift into the expected line. */
static char *env_path_set(const char *dir)
{
    const char *prev = getenv("PATH");
    char *saved = prev ? xstrdup(prev) : NULL;
    setenv("PATH", dir, 1);
    return saved;
}

static void env_path_restore(char *saved)
{
    if (saved) {
        setenv("PATH", saved, 1);
        free(saved);
    } else {
        unsetenv("PATH");
    }
}

/* ---------- Environment section ---------- */

static void test_agent_env_section_present_by_default(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    char *p = agent_env_build_suffix("claude-test-1");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "# Environment\n\n"));
        EXPECT(contains(p, "- Working directory: ~\n"));
        char *home = xasprintf("- Home directory: %s\n", s.root);
        EXPECT(contains(p, home));
        free(home);
        EXPECT(contains(p, "- Operating system: "));
        EXPECT(contains(p, "- Command shell: "));
        EXPECT(contains(p, "- Model: claude-test-1\n"));
        EXPECT(contains(p, "- Git repository: no\n"));
        EXPECT(!contains(p, "<env>"));
        free(p);
    }
    sandbox_free(&s);
}

static void test_agent_env_section_git_root(void)
{
    struct sandbox s;
    sandbox_init(&s);
    sandbox_mkdir(&s, ".git");
    SANDBOX_CHDIR(&s, ".");
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "- Git repository root: ~\n"));
        free(p);
    }
    sandbox_free(&s);
}

static void test_agent_env_section_git_root_from_subdir(void)
{
    struct sandbox s;
    sandbox_init(&s);
    /* .git lives at the sandbox root; cwd is two levels deeper. The
     * Environment section should report that root using the same upward walk
     * as AGENTS.md. */
    sandbox_mkdir(&s, ".git");
    sandbox_mkdir(&s, "a/b");
    SANDBOX_CHDIR(&s, "a/b");
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "- Git repository root: ~\n"));
        free(p);
    }
    sandbox_free(&s);
}

static void test_agent_env_section_omits_model_when_null(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    char *p = agent_env_build_suffix(NULL);
    EXPECT(p != NULL);
    if (p) {
        EXPECT(!contains(p, "- Model:"));
        free(p);
    }
    sandbox_free(&s);
}

static void test_agent_env_section_omits_home_when_unset(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    unsetenv("HOME");
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(!contains(p, "- Home directory:"));
        char *cwd = xasprintf("- Working directory: %s\n", s.root);
        EXPECT(contains(p, cwd));
        free(cwd);
        free(p);
    }
    sandbox_free(&s);
}

static void test_agent_env_section_reports_command_shell(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_BASH_SHELL", "/bin/sh", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "- Command shell: /bin/sh\n"));
        free(p);
    }
    sandbox_free(&s);
}

static void test_no_env_knob_disables_section(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    /* No env, no AGENTS.md → NULL. */
    EXPECT(p == NULL);
    free(p);
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_both_knobs_disable_returns_null(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);
    setenv("HAX_NO_AGENTS_MD", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p == NULL);
    free(p);
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
    sandbox_free(&s);
}

/* ---------- commands probe ---------- */

static void test_commands_line_lists_present(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    sandbox_stage_command(&s, "bin", "rg");
    sandbox_stage_command(&s, "bin", "jq");
    char *bin = sandbox_path(&s, "bin");
    char *saved = env_path_set(bin);
    free(bin);

    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "Available command-line tools: `rg`, `jq`.\n"));
        EXPECT(contains(p, "Prefer `rg` to `grep -r`.\n"));
        free(p);
    }

    env_path_restore(saved);
    sandbox_free(&s);
}

static void test_commands_line_omitted_when_none(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    /* Empty (but valid) PATH dir → none of the probed commands present. */
    sandbox_mkdir(&s, "empty-bin");
    char *empty_bin = sandbox_path(&s, "empty-bin");
    char *saved = env_path_set(empty_bin);
    free(empty_bin);

    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(!contains(p, "Available command-line tools:"));
        EXPECT(!contains(p, "Prefer `"));
        free(p);
    }

    env_path_restore(saved);
    sandbox_free(&s);
}

static void test_commands_line_skips_relative_path_entries(void)
{
    /* PATH entries that are relative (`.`, `bin`, …) refer to cwd, which
     * may be a checkout of someone else's project. Advertising a binary
     * picked up from a relative PATH entry could steer the model toward
     * a repo-provided executable that shadows the host utility. Stage a
     * fake `rg` directly in cwd, set PATH=., expect it NOT to appear. */
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    /* Drop the fake straight into cwd, no subdir — `.` resolves here. */
    sandbox_stage_command(&s, ".", "rg");
    char *saved = env_path_set(".");

    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(!contains(p, "Available command-line tools:"));
        EXPECT(!contains(p, "Prefer `"));
        EXPECT(!contains(p, "`rg`"));
        free(p);
    }

    env_path_restore(saved);
    sandbox_free(&s);
}

static void test_commands_line_ignores_directories(void)
{
    /* access(X_OK) returns success for searchable directories, so a PATH
     * entry containing a `rg/` subdirectory must not be advertised as the
     * `rg` command. Stage a directory named like a probed command and a
     * real fake executable for an unrelated probed command, expect only
     * the real one to land in the line. */
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    sandbox_mkdir(&s, "bin/rg");
    sandbox_stage_command(&s, "bin", "jq");
    char *bin = sandbox_path(&s, "bin");
    char *saved = env_path_set(bin);
    free(bin);

    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "Available command-line tools: `jq`.\n"));
        EXPECT(!contains(p, "Prefer `"));
        EXPECT(!contains(p, "`rg`"));
        free(p);
    }

    env_path_restore(saved);
    sandbox_free(&s);
}

static void test_commands_line_preserves_canonical_order(void)
{
    /* Probed list is rg, fd, jq, gh, python3, node — line should follow
     * that order regardless of which subset is present. */
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    sandbox_stage_command(&s, "bin", "node");
    sandbox_stage_command(&s, "bin", "python3");
    sandbox_stage_command(&s, "bin", "fd");
    sandbox_stage_command(&s, "bin", "rg");
    sandbox_stage_command(&s, "bin", "gh");
    char *bin = sandbox_path(&s, "bin");
    char *saved = env_path_set(bin);
    free(bin);

    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "Available command-line tools: `rg`, `fd`, `gh`, `python3`, `node`.\n"));
        EXPECT(contains(p, "Prefer `rg` to `grep -r`, `fd` to `find`, `python3` to "
                           "`python`.\n"));
        free(p);
    }

    env_path_restore(saved);
    sandbox_free(&s);
}

/* ---------- AGENTS.md walk ---------- */

static void test_agents_md_cwd_only_no_root_marker(void)
{
    struct sandbox s;
    sandbox_init(&s);
    /* Place an AGENTS.md two levels above cwd, but no .git anywhere. We
     * expect the walk NOT to pick up the parent file — only cwd-level
     * (which is absent here) is considered. */
    sandbox_write(&s, "AGENTS.md", "# outer\nshould-not-appear\n");
    sandbox_mkdir(&s, "sub/dir");
    SANDBOX_CHDIR(&s, "sub/dir");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    /* Without .git anywhere, the parent file is ignored and there is no
     * cwd-level file → nothing to emit, suffix is NULL. */
    EXPECT(p == NULL);
    free(p);
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_agents_md_walks_to_git_root_farthest_first(void)
{
    struct sandbox s;
    sandbox_init(&s);
    /* Tree:
     *   $root/.git/
     *   $root/AGENTS.md           ← outer (project root)
     *   $root/a/AGENTS.md         ← middle
     *   $root/a/b/AGENTS.md       ← inner (cwd)
     * Expected emit order: outer, middle, inner — closest last. */
    sandbox_mkdir(&s, ".git");
    sandbox_write(&s, "AGENTS.md", "OUTER_MARKER\n");
    sandbox_write(&s, "a/AGENTS.md", "MIDDLE_MARKER\n");
    sandbox_write(&s, "a/b/AGENTS.md", "INNER_MARKER\n");
    SANDBOX_CHDIR(&s, "a/b");

    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        const char *o = strstr(p, "OUTER_MARKER");
        const char *m = strstr(p, "MIDDLE_MARKER");
        const char *n = strstr(p, "INNER_MARKER");
        EXPECT(o && m && n);
        if (o && m && n) {
            EXPECT(o < m);
            EXPECT(m < n);
        }
        EXPECT(contains(p, "# Project Context"));
        EXPECT(contains(p, "Project guidance below overrides the assistant defaults above."));
        EXPECT(contains(p, "## "));
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_agents_md_global_first(void)
{
    struct sandbox s;
    sandbox_init(&s);
    /* HOME is sandboxed; create the global file there. */
    sandbox_write(&s, ".config/hax/AGENTS.md", "GLOBAL_MARKER\n");

    /* And a project-local file under a .git'd root. */
    sandbox_mkdir(&s, "proj/.git");
    sandbox_write(&s, "proj/AGENTS.md", "LOCAL_MARKER\n");
    SANDBOX_CHDIR(&s, "proj");

    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        const char *g = strstr(p, "GLOBAL_MARKER");
        const char *l = strstr(p, "LOCAL_MARKER");
        EXPECT(g && l);
        if (g && l)
            EXPECT(g < l);
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_no_agents_md_knob_disables_walk(void)
{
    struct sandbox s;
    sandbox_init(&s);
    sandbox_mkdir(&s, ".git");
    sandbox_write(&s, "AGENTS.md", "SHOULD_NOT_APPEAR\n");
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_AGENTS_MD", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(!contains(p, "SHOULD_NOT_APPEAR"));
        EXPECT(!contains(p, "# Project Context"));
        EXPECT(contains(p, "# Environment"));
        free(p);
    }
    unsetenv("HAX_NO_AGENTS_MD");
    sandbox_free(&s);
}

static void test_xdg_config_home_overrides_home(void)
{
    struct sandbox s;
    sandbox_init(&s);
    /* Two candidate global locations: HOME-based one (via sandbox_init) and an
     * explicit XDG_CONFIG_HOME pointing elsewhere. The XDG one must win. */
    sandbox_write(&s, ".config/hax/AGENTS.md", "HOME_GLOBAL\n");
    sandbox_write(&s, "xdg/hax/AGENTS.md", "XDG_GLOBAL\n");
    char *xdg_root = sandbox_path(&s, "xdg");
    setenv("XDG_CONFIG_HOME", xdg_root, 1);
    free(xdg_root);

    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "XDG_GLOBAL"));
        EXPECT(!contains(p, "HOME_GLOBAL"));
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    unsetenv("XDG_CONFIG_HOME");
    sandbox_free(&s);
}

static void test_agents_md_invalid_bytes_sanitized(void)
{
    struct sandbox s;
    sandbox_init(&s);
    /* AGENTS.md with an embedded NUL and an invalid UTF-8 byte. The raw
     * bytes would truncate the prompt under strlen and Jansson would
     * reject the request as non-UTF-8 — utf8_sanitize must replace both
     * with U+FFFD before they enter the buffer. */
    sandbox_mkdir(&s, ".git");
    const char dirty[] = "before\0middle\xFF"
                         "after\n";
    sandbox_write_bytes(&s, "AGENTS.md", dirty, sizeof(dirty) - 1);
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        /* Both `before` (pre-NUL) and `after` (post-replacement) survive,
         * which proves the prompt didn't get truncated mid-file. */
        EXPECT(contains(p, "before"));
        EXPECT(contains(p, "middle"));
        EXPECT(contains(p, "after"));
        /* No raw NUL anywhere in the C string (strlen would already cut
         * the buffer at one) and no raw 0xFF byte. */
        EXPECT(strlen(p) > strlen("before") + strlen("middle") + strlen("after"));
        EXPECT(memchr(p, '\xFF', strlen(p)) == NULL);
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

/* ---------- skills ---------- */

static void test_skills_none(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    /* No AGENTS.md, no skills, env disabled → NULL. */
    EXPECT(p == NULL);
    free(p);
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_skills_with_description_sorted(void)
{
    struct sandbox s;
    sandbox_init(&s);
    /* Two skills with frontmatter; verify sorted output and that the
     * description field is parsed and emitted. */
    sandbox_write(&s, ".agents/skills/zeta/SKILL.md",
                  "---\nname: zeta\ndescription: zeta does Z\n---\n# Zeta\n\nbody\n");
    sandbox_write(&s, ".agents/skills/alpha/SKILL.md",
                  "---\nname: alpha\ndescription: \"alpha does A\"\n---\nbody\n");
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "# Skills"));
        /* sandbox_init pins HOME=sandbox root, so absolute project paths collapse
         * to `~/.agents/skills/...`. */
        EXPECT(contains(p, "- alpha: alpha does A (~/.agents/skills/alpha/SKILL.md)"));
        EXPECT(contains(p, "- zeta: zeta does Z (~/.agents/skills/zeta/SKILL.md)"));
        const char *a = strstr(p, "- alpha");
        const char *z = strstr(p, "- zeta");
        EXPECT(a && z && a < z);
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_skills_crlf_frontmatter(void)
{
    struct sandbox s;
    sandbox_init(&s);
    /* Files checked out on Windows-style line endings have CRLF
     * everywhere, including the opening `---` fence. The closer already
     * accepts \r — verify the opener does too, otherwise the description
     * silently goes missing for these files. */
    const char body[] = "---\r\ndescription: from crlf\r\n---\r\nbody\r\n";
    sandbox_write_bytes(&s, ".agents/skills/crlf/SKILL.md", body, sizeof(body) - 1);
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "- crlf: from crlf (~/.agents/skills/crlf/SKILL.md)"));
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_skills_no_frontmatter_falls_back_to_dir(void)
{
    struct sandbox s;
    sandbox_init(&s);
    sandbox_write(&s, ".agents/skills/raw/SKILL.md", "Just a body, no frontmatter at all.\n");
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "- raw (~/.agents/skills/raw/SKILL.md)"));
        EXPECT(!contains(p, "raw:")); /* no description → no colon-and-text */
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_skills_dir_without_skill_md_skipped(void)
{
    struct sandbox s;
    sandbox_init(&s);
    /* Subdir exists but has no SKILL.md inside — must be skipped. */
    sandbox_mkdir(&s, ".agents/skills/empty");
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    /* Nothing valid → NULL. */
    EXPECT(p == NULL);
    free(p);
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_skills_global_root(void)
{
    struct sandbox s;
    sandbox_init(&s);
    /* Global skill via $HOME/.config/hax/skills (HOME is sandboxed). */
    sandbox_write(&s, ".config/hax/skills/sample/SKILL.md", "---\ndescription: from global\n---\n");
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "- sample: from global"));
        /* Global path is absolute, embedded under the sandbox root. */
        EXPECT(contains(p, "/.config/hax/skills/sample/SKILL.md"));
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_skills_project_shadows_global(void)
{
    struct sandbox s;
    sandbox_init(&s);
    sandbox_write(&s, ".config/hax/skills/dup/SKILL.md", "---\ndescription: from global\n---\n");
    /* The project lives below $HOME rather than at it: with cwd equal to $HOME
     * the project root and `~/.agents/skills` would be the same directory, and
     * this would no longer be a project-versus-global test. */
    sandbox_write(&s, "proj/.agents/skills/dup/SKILL.md", "---\ndescription: from project\n---\n");
    SANDBOX_CHDIR(&s, "proj");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "from project"));
        EXPECT(!contains(p, "from global"));
        /* And exactly one entry for `dup`. */
        const char *first = strstr(p, "- dup");
        EXPECT(first != NULL);
        if (first)
            EXPECT(strstr(first + 1, "- dup") == NULL);
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_skills_disabled_by_no_skills(void)
{
    struct sandbox s;
    sandbox_init(&s);
    sandbox_write(&s, ".agents/skills/foo/SKILL.md", "---\ndescription: hidden\n---\n");
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_SKILLS", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL); /* Environment section still present */
    if (p) {
        EXPECT(!contains(p, "# Skills"));
        EXPECT(!contains(p, "hidden"));
        free(p);
    }
    unsetenv("HAX_NO_SKILLS");
    sandbox_free(&s);
}

static void test_skills_walk_up_to_project_root(void)
{
    struct sandbox s;
    sandbox_init(&s);
    /* .git at $root/proj; skills at the project root; cwd two levels deeper. */
    sandbox_mkdir(&s, "proj/.git");
    sandbox_write(&s, "proj/.agents/skills/rooted/SKILL.md",
                  "---\ndescription: from project root\n---\n");
    sandbox_mkdir(&s, "proj/a/b");
    SANDBOX_CHDIR(&s, "proj/a/b");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "- rooted: from project root"));
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_skills_nearer_dir_shadows_project_root(void)
{
    struct sandbox s;
    sandbox_init(&s);
    sandbox_mkdir(&s, "proj/.git");
    sandbox_write(&s, "proj/.agents/skills/dup/SKILL.md", "---\ndescription: from root\n---\n");
    sandbox_write(&s, "proj/a/.agents/skills/dup/SKILL.md", "---\ndescription: from subdir\n---\n");
    SANDBOX_CHDIR(&s, "proj/a");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "from subdir"));
        EXPECT(!contains(p, "from root"));
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_skills_no_root_marker_stays_in_cwd(void)
{
    struct sandbox s;
    sandbox_init(&s);
    /* No .git anywhere: a parent's skills must not be pulled in. HOME is the
     * sandbox root, so stage the parent skills one level below it to keep the
     * `~/.agents/skills` root out of this. */
    sandbox_write(&s, "w/.agents/skills/stray/SKILL.md", "---\ndescription: from parent\n---\n");
    sandbox_mkdir(&s, "w/a");
    SANDBOX_CHDIR(&s, "w/a");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p == NULL);
    free(p);
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_skills_shared_agents_root(void)
{
    struct sandbox s;
    sandbox_init(&s);
    /* HOME is the sandbox root, so this is `~/.agents/skills`. cwd is a
     * separate project root, so the upward walk stops before reaching it. */
    sandbox_write(&s, ".agents/skills/shared/SKILL.md", "---\ndescription: from shared\n---\n");
    sandbox_mkdir(&s, "proj/.git");
    SANDBOX_CHDIR(&s, "proj");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "- shared: from shared"));
        EXPECT(contains(p, "~/.agents/skills/shared/SKILL.md"));
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_skills_hax_global_shadows_shared(void)
{
    struct sandbox s;
    sandbox_init(&s);
    sandbox_write(&s, ".agents/skills/dup/SKILL.md", "---\ndescription: from shared\n---\n");
    sandbox_write(&s, ".config/hax/skills/dup/SKILL.md",
                  "---\ndescription: from hax global\n---\n");
    sandbox_mkdir(&s, "proj/.git");
    SANDBOX_CHDIR(&s, "proj");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "from hax global"));
        EXPECT(!contains(p, "from shared"));
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

/* With cwd at $HOME the project walk reaches `~/.agents/skills` itself. It must
 * still rank below `~/.config/hax/skills`, so that standing in $HOME does not
 * reorder two global roots. */
static void test_skills_hax_global_shadows_shared_at_home(void)
{
    struct sandbox s;
    sandbox_init(&s);
    sandbox_write(&s, ".agents/skills/dup/SKILL.md", "---\ndescription: from shared\n---\n");
    sandbox_write(&s, ".config/hax/skills/dup/SKILL.md",
                  "---\ndescription: from hax global\n---\n");
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "from hax global"));
        EXPECT(!contains(p, "from shared"));
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

/* $HOME reaching the sandbox through a symlink while getcwd() reports the
 * physical path: the two spellings of `~/.agents/skills` must still be
 * recognized as one directory, or the walk would collect it early and reorder
 * the global roots again. */
static void test_skills_hax_global_shadows_shared_symlinked_home(void)
{
    struct sandbox s;
    sandbox_init(&s);
    sandbox_mkdir(&s, "real");
    char *real = sandbox_path(&s, "real");
    char *link = sandbox_path(&s, "link");
    int linked = symlink(real, link) == 0;
    free(real);
    if (!linked) {
        free(link);
        sandbox_free(&s);
        T_SKIP("symlink unsupported");
    }
    /* Both paths name the same directory; write through the physical one. */
    sandbox_write(&s, "real/.agents/skills/dup/SKILL.md", "---\ndescription: from shared\n---\n");
    sandbox_write(&s, "real/.config/hax/skills/dup/SKILL.md",
                  "---\ndescription: from hax global\n---\n");

    setenv("HOME", link, 1);
    free(link);
    SANDBOX_CHDIR(&s, "real");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "from hax global"));
        EXPECT(!contains(p, "from shared"));
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

/* The held-back shared root must still be collected: a skill that exists only
 * in `~/.agents/skills` stays visible with cwd at $HOME. */
static void test_skills_shared_root_survives_at_home(void)
{
    struct sandbox s;
    sandbox_init(&s);
    sandbox_write(&s, ".agents/skills/only/SKILL.md", "---\ndescription: from shared\n---\n");
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "- only: from shared"));
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_skills_survive_no_agents_md(void)
{
    /* The gates are orthogonal: suppressing AGENTS.md must not take the
     * skills listing with it. */
    struct sandbox s;
    sandbox_init(&s);
    sandbox_write(&s, ".agents/skills/foo/SKILL.md", "---\ndescription: still here\n---\n");
    sandbox_write(&s, "AGENTS.md", "project rules\n");
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_AGENTS_MD", "1", 1);
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(!contains(p, "project rules"));
        EXPECT(contains(p, "# Skills"));
        EXPECT(contains(p, "still here"));
        free(p);
    }
    unsetenv("HAX_NO_AGENTS_MD");
    sandbox_free(&s);
}

/* ---------- subagents section ---------- */

static void test_subagents_section_and_presets(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    unsetenv("HAX_NO_SUBAGENTS");
    unsetenv("HAX_NO_TASKS");

    /* Present by default, with the conservative framing, and placed before
     * the Environment section — it's hax-level instruction, not project
     * context. The tasks section leads because subagent guidance builds on
     * task_wait. With no presets defined, --preset isn't advertised at all. */
    char *p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "# Subagents"));
        EXPECT(contains(p, "only when the user asks"));
        EXPECT(contains(p, "task_wait"));
        /* With no presets defined, --preset isn't advertised at all. */
        EXPECT(!contains(p, "--preset"));
        const char *tasks = strstr(p, "# Background tasks");
        const char *sub = strstr(p, "# Subagents");
        const char *env = strstr(p, "# Environment");
        EXPECT(tasks && sub && env && tasks < sub && sub < env);
        free(p);
    }

    /* With tasks disabled, the subagents section falls back to synchronous
     * guidance and the tasks section disappears. */
    setenv("HAX_NO_TASKS", "1", 1);
    p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(!contains(p, "# Background tasks"));
        EXPECT(contains(p, "# Subagents"));
        EXPECT(contains(p, "timeout_seconds (e.g. 1800)"));
        EXPECT(!contains(p, "task_wait"));
        free(p);
    }
    unsetenv("HAX_NO_TASKS");

    /* Described presets are listed sorted under a lead-in that names the
     * flag. A description-less preset is a favorite, not a role: its bare
     * name is never advertised. A preset naming a provider the registry
     * can't resolve is never advertised either — it would fail on every
     * invocation. */
    EXPECT(config_load("{\"presets\": {"
                       "\"zeta\": {\"provider\": \"mock\", \"model\": \"m2\"},"
                       "\"typo\": {\"provider\": \"does-not-exist\", "
                       "\"description\": \"broken role\"},"
                       "\"review\": {\"provider\": \"mock\", "
                       "\"description\": \"code review stance\"},"
                       "\"alpha\": {\"provider\": \"mock\", "
                       "\"description\": \"quick answers\"}}}") == 0);
    p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "Presets (select with `--preset <name>`):\n"
                           "- alpha: quick answers\n- review: code review stance\n"));
        EXPECT(!contains(p, "zeta"));
        EXPECT(!contains(p, "typo"));
        free(p);
    }

    /* Nothing advertisable (unknown providers, description-less favorites):
     * the heading — the thing that advertises --preset — must not appear
     * either, or the model would be invited to guess a name with no valid
     * value to pass. */
    EXPECT(config_load("{\"presets\": {"
                       "\"a\": {\"provider\": \"does-not-exist\", "
                       "\"description\": \"broken\"},"
                       "\"b\": {\"provider\": \"mock\"}}}") == 0);
    p = agent_env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(!contains(p, "Presets ("));
        EXPECT(!contains(p, "--preset"));
        free(p);
    }
    config_load(NULL);

    setenv("HAX_NO_SUBAGENTS", "1", 1);
    setenv("HAX_NO_TASKS", "1", 1);
    sandbox_free(&s);
}

int main(void)
{
    test_agent_env_section_present_by_default();
    test_agent_env_section_git_root();
    test_agent_env_section_git_root_from_subdir();
    test_agent_env_section_omits_model_when_null();
    test_agent_env_section_omits_home_when_unset();
    test_agent_env_section_reports_command_shell();
    test_no_env_knob_disables_section();
    test_both_knobs_disable_returns_null();

    test_commands_line_lists_present();
    test_commands_line_omitted_when_none();
    test_commands_line_skips_relative_path_entries();
    test_commands_line_ignores_directories();
    test_commands_line_preserves_canonical_order();

    test_agents_md_cwd_only_no_root_marker();
    test_agents_md_walks_to_git_root_farthest_first();
    test_agents_md_global_first();
    test_no_agents_md_knob_disables_walk();
    test_xdg_config_home_overrides_home();
    test_agents_md_invalid_bytes_sanitized();

    test_skills_none();
    test_skills_with_description_sorted();
    test_skills_crlf_frontmatter();
    test_skills_no_frontmatter_falls_back_to_dir();
    test_skills_dir_without_skill_md_skipped();
    test_skills_global_root();
    test_skills_project_shadows_global();
    test_skills_walk_up_to_project_root();
    test_skills_nearer_dir_shadows_project_root();
    test_skills_no_root_marker_stays_in_cwd();
    test_skills_shared_agents_root();
    test_skills_hax_global_shadows_shared();
    test_skills_hax_global_shadows_shared_at_home();
    test_skills_hax_global_shadows_shared_symlinked_home();
    test_skills_shared_root_survives_at_home();
    test_skills_disabled_by_no_skills();
    test_skills_survive_no_agents_md();
    test_subagents_section_and_presets();

    T_REPORT();
}
