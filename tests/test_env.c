/* SPDX-License-Identifier: MIT */
#include "env.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "harness.h"
#include "util.h"

/* Each test stages a fresh tmpdir tree, chdirs into it, runs
 * env_build_suffix(), then chdirs back and rm -rfs the tree. We also pin
 * HOME and XDG_CONFIG_HOME inside the sandbox so the developer's real
 * ~/.config/hax/AGENTS.md doesn't leak into test output, and clear the
 * HAX_NO_* knobs unless a test sets them deliberately. */

struct sandbox {
    char *root;     /* mkdtemp result; everything else lives under this */
    char *prev_cwd; /* cwd to restore on cleanup */
};

static void sb_init(struct sandbox *s)
{
    s->prev_cwd = getcwd(NULL, 0);
    s->root = xstrdup("/tmp/hax-env-test-XXXXXX");
    if (!mkdtemp(s->root)) {
        FAIL("mkdtemp: %s", strerror(errno));
        free(s->root);
        s->root = NULL;
    }
    /* Point HOME and XDG_CONFIG_HOME at the sandbox so global AGENTS.md
     * lookups don't escape it. Tests that want a global file create it
     * under $HOME/.config/hax/AGENTS.md. */
    setenv("HOME", s->root, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
}

static void sb_free(struct sandbox *s)
{
    if (s->prev_cwd) {
        if (chdir(s->prev_cwd) != 0)
            FAIL("chdir(prev_cwd=%s): %s", s->prev_cwd, strerror(errno));
        free(s->prev_cwd);
    }
    if (s->root) {
        char *cmd = xasprintf("rm -rf '%s'", s->root);
        int rc = system(cmd);
        (void)rc;
        free(cmd);
        free(s->root);
    }
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

/* ---------- env block ---------- */

static void test_env_block_present_by_default(void)
{
    struct sandbox s;
    sb_init(&s);
    if (chdir(s.root) != 0) {
        FAIL("chdir: %s", strerror(errno));
        sb_free(&s);
        return;
    }
    char *p = env_build_suffix("claude-test-1");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "<env>"));
        EXPECT(contains(p, "</env>"));
        EXPECT(contains(p, "  cwd: "));
        EXPECT(contains(p, "  os: "));
        EXPECT(contains(p, "  shell: "));
        EXPECT(contains(p, "  model: claude-test-1"));
        EXPECT(contains(p, "  is_git_repo: no"));
        free(p);
    }
    sb_free(&s);
}

static void test_env_block_git_repo_yes(void)
{
    struct sandbox s;
    sb_init(&s);
    char *git = xasprintf("%s/.git", s.root);
    mkdir(git, 0755);
    free(git);
    if (chdir(s.root) != 0) {
        FAIL("chdir: %s", strerror(errno));
        sb_free(&s);
        return;
    }
    char *p = env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "  is_git_repo: yes"));
        free(p);
    }
    sb_free(&s);
}

static void test_env_block_git_repo_yes_from_subdir(void)
{
    struct sandbox s;
    sb_init(&s);
    /* .git lives at the sandbox root; cwd is two levels deeper. The env
     * block should still report is_git_repo: yes — same upward walk that
     * AGENTS.md uses, so the two stay consistent. */
    char *git = xasprintf("%s/.git", s.root);
    mkdir(git, 0755);
    free(git);
    char *sub = xasprintf("%s/a/b", s.root);
    mkdirs(sub);
    if (chdir(sub) != 0) {
        FAIL("chdir(%s): %s", sub, strerror(errno));
        free(sub);
        sb_free(&s);
        return;
    }
    free(sub);
    char *p = env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "  is_git_repo: yes"));
        free(p);
    }
    sb_free(&s);
}

static void test_env_block_omits_model_when_null(void)
{
    struct sandbox s;
    sb_init(&s);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    char *p = env_build_suffix(NULL);
    EXPECT(p != NULL);
    if (p) {
        EXPECT(!contains(p, "  model:"));
        free(p);
    }
    sb_free(&s);
}

static void test_no_env_knob_disables_block(void)
{
    struct sandbox s;
    sb_init(&s);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    setenv("HAX_NO_ENV", "1", 1);
    char *p = env_build_suffix("m");
    /* No env, no AGENTS.md → NULL. */
    EXPECT(p == NULL);
    free(p);
    unsetenv("HAX_NO_ENV");
    sb_free(&s);
}

static void test_both_knobs_disable_returns_null(void)
{
    struct sandbox s;
    sb_init(&s);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    setenv("HAX_NO_ENV", "1", 1);
    setenv("HAX_NO_AGENTS_MD", "1", 1);
    char *p = env_build_suffix("m");
    EXPECT(p == NULL);
    free(p);
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
    sb_free(&s);
}

/* ---------- commands probe ---------- */

/* Stage a fake executable named `name` under `dir` (creating `dir`).
 * Content is irrelevant — env.c only checks access(X_OK). */
static void stage_fake_command(const char *dir, const char *name)
{
    mkdirs(dir);
    char *path = xasprintf("%s/%s", dir, name);
    write_file(path, "#!/bin/sh\n");
    if (chmod(path, 0755) != 0)
        FAIL("chmod(%s): %s", path, strerror(errno));
    free(path);
}

static void test_commands_line_lists_present(void)
{
    struct sandbox s;
    sb_init(&s);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    char *bin = xasprintf("%s/bin", s.root);
    stage_fake_command(bin, "rg");
    stage_fake_command(bin, "jq");
    /* PATH = sandbox bin only. Real /usr/bin tools (gh, python3, etc.)
     * are then deliberately invisible, pinning the expected line. */
    char *prev_path = getenv("PATH");
    char *saved = prev_path ? xstrdup(prev_path) : NULL;
    setenv("PATH", bin, 1);

    char *p = env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        /* rg has a replacement annotation (grep -r); jq doesn't. */
        EXPECT(contains(p, "  preferred_commands: rg (instead of grep -r), jq\n"));
        free(p);
    }

    if (saved) {
        setenv("PATH", saved, 1);
        free(saved);
    } else {
        unsetenv("PATH");
    }
    free(bin);
    sb_free(&s);
}

static void test_commands_line_omitted_when_none(void)
{
    struct sandbox s;
    sb_init(&s);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    /* Empty (but valid) PATH dir → none of the probed commands present. */
    char *empty_bin = xasprintf("%s/empty-bin", s.root);
    mkdirs(empty_bin);
    char *prev_path = getenv("PATH");
    char *saved = prev_path ? xstrdup(prev_path) : NULL;
    setenv("PATH", empty_bin, 1);

    char *p = env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(!contains(p, "preferred_commands:"));
        free(p);
    }

    if (saved) {
        setenv("PATH", saved, 1);
        free(saved);
    } else {
        unsetenv("PATH");
    }
    free(empty_bin);
    sb_free(&s);
}

static void test_commands_line_skips_relative_path_entries(void)
{
    /* PATH entries that are relative (`.`, `bin`, …) refer to cwd, which
     * may be a checkout of someone else's project. Advertising a binary
     * picked up from a relative PATH entry could steer the model toward
     * a repo-provided executable that shadows the host utility. Stage a
     * fake `rg` directly in cwd, set PATH=., expect it NOT to appear. */
    struct sandbox s;
    sb_init(&s);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    /* Drop the fake straight into cwd, no subdir — `.` resolves here. */
    stage_fake_command(s.root, "rg");
    char *prev_path = getenv("PATH");
    char *saved = prev_path ? xstrdup(prev_path) : NULL;
    setenv("PATH", ".", 1);

    char *p = env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(!contains(p, "preferred_commands:"));
        EXPECT(!contains(p, "rg"));
        free(p);
    }

    if (saved) {
        setenv("PATH", saved, 1);
        free(saved);
    } else {
        unsetenv("PATH");
    }
    sb_free(&s);
}

static void test_commands_line_ignores_directories(void)
{
    /* access(X_OK) returns success for searchable directories, so a PATH
     * entry containing a `rg/` subdirectory must not be advertised as the
     * `rg` command. Stage a directory named like a probed command and a
     * real fake executable for an unrelated probed command, expect only
     * the real one to land in the line. */
    struct sandbox s;
    sb_init(&s);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    char *bin = xasprintf("%s/bin", s.root);
    char *rg_as_dir = xasprintf("%s/rg", bin);
    mkdirs(rg_as_dir);
    stage_fake_command(bin, "jq");
    char *prev_path = getenv("PATH");
    char *saved = prev_path ? xstrdup(prev_path) : NULL;
    setenv("PATH", bin, 1);

    char *p = env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "  preferred_commands: jq\n"));
        EXPECT(!contains(p, "rg"));
        free(p);
    }

    if (saved) {
        setenv("PATH", saved, 1);
        free(saved);
    } else {
        unsetenv("PATH");
    }
    free(rg_as_dir);
    free(bin);
    sb_free(&s);
}

static void test_commands_line_preserves_canonical_order(void)
{
    /* Probed list is rg, fd, jq, gh, python3, node — line should follow
     * that order regardless of which subset is present. */
    struct sandbox s;
    sb_init(&s);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    char *bin = xasprintf("%s/bin", s.root);
    stage_fake_command(bin, "node");
    stage_fake_command(bin, "rg");
    stage_fake_command(bin, "gh");
    char *prev_path = getenv("PATH");
    char *saved = prev_path ? xstrdup(prev_path) : NULL;
    setenv("PATH", bin, 1);

    char *p = env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "  preferred_commands: rg (instead of grep -r), gh, node\n"));
        free(p);
    }

    if (saved) {
        setenv("PATH", saved, 1);
        free(saved);
    } else {
        unsetenv("PATH");
    }
    free(bin);
    sb_free(&s);
}

/* ---------- AGENTS.md walk ---------- */

static void test_agents_md_cwd_only_no_root_marker(void)
{
    struct sandbox s;
    sb_init(&s);
    /* Place an AGENTS.md two levels above cwd, but no .git anywhere. We
     * expect the walk NOT to pick up the parent file — only cwd-level
     * (which is absent here) is considered. */
    char *outer_md = xasprintf("%s/AGENTS.md", s.root);
    write_file(outer_md, "# outer\nshould-not-appear\n");
    free(outer_md);
    char *inner = xasprintf("%s/sub/dir", s.root);
    mkdirs(inner);
    if (chdir(inner) != 0) {
        FAIL("chdir(%s): %s", inner, strerror(errno));
        free(inner);
        sb_free(&s);
        return;
    }
    free(inner);
    setenv("HAX_NO_ENV", "1", 1);
    char *p = env_build_suffix("m");
    /* Without .git anywhere, the parent file is ignored and there is no
     * cwd-level file → nothing to emit, suffix is NULL. */
    EXPECT(p == NULL);
    free(p);
    unsetenv("HAX_NO_ENV");
    sb_free(&s);
}

static void test_agents_md_walks_to_git_root_farthest_first(void)
{
    struct sandbox s;
    sb_init(&s);
    /* Tree:
     *   $root/.git/
     *   $root/AGENTS.md           ← outer (project root)
     *   $root/a/AGENTS.md         ← middle
     *   $root/a/b/AGENTS.md       ← inner (cwd)
     * Expected emit order: outer, middle, inner — closest last. */
    char *git = xasprintf("%s/.git", s.root);
    mkdir(git, 0755);
    free(git);

    char *outer = xasprintf("%s/AGENTS.md", s.root);
    write_file(outer, "OUTER_MARKER\n");
    free(outer);

    char *middle_dir = xasprintf("%s/a", s.root);
    mkdirs(middle_dir);
    char *middle = xasprintf("%s/AGENTS.md", middle_dir);
    write_file(middle, "MIDDLE_MARKER\n");
    free(middle);
    free(middle_dir);

    char *inner_dir = xasprintf("%s/a/b", s.root);
    mkdirs(inner_dir);
    char *inner = xasprintf("%s/AGENTS.md", inner_dir);
    write_file(inner, "INNER_MARKER\n");
    free(inner);

    if (chdir(inner_dir) != 0) {
        FAIL("chdir(%s): %s", inner_dir, strerror(errno));
        free(inner_dir);
        sb_free(&s);
        return;
    }
    free(inner_dir);

    setenv("HAX_NO_ENV", "1", 1);
    char *p = env_build_suffix("m");
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
        EXPECT(contains(p, "## "));
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sb_free(&s);
}

static void test_agents_md_global_first(void)
{
    struct sandbox s;
    sb_init(&s);
    /* HOME is sandboxed; create the global file there. */
    char *global = xasprintf("%s/.config/hax/AGENTS.md", s.root);
    write_file(global, "GLOBAL_MARKER\n");
    free(global);

    /* And a project-local file under a .git'd root. */
    char *git = xasprintf("%s/proj/.git", s.root);
    mkdirs(git);
    free(git);
    char *local = xasprintf("%s/proj/AGENTS.md", s.root);
    write_file(local, "LOCAL_MARKER\n");
    free(local);

    char *cwd = xasprintf("%s/proj", s.root);
    if (chdir(cwd) != 0) {
        FAIL("chdir(%s): %s", cwd, strerror(errno));
        free(cwd);
        sb_free(&s);
        return;
    }
    free(cwd);

    setenv("HAX_NO_ENV", "1", 1);
    char *p = env_build_suffix("m");
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
    sb_free(&s);
}

static void test_no_agents_md_knob_disables_walk(void)
{
    struct sandbox s;
    sb_init(&s);
    char *git = xasprintf("%s/.git", s.root);
    mkdir(git, 0755);
    free(git);
    char *md = xasprintf("%s/AGENTS.md", s.root);
    write_file(md, "SHOULD_NOT_APPEAR\n");
    free(md);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    setenv("HAX_NO_AGENTS_MD", "1", 1);
    char *p = env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(!contains(p, "SHOULD_NOT_APPEAR"));
        EXPECT(!contains(p, "# Project Context"));
        EXPECT(contains(p, "<env>"));
        free(p);
    }
    unsetenv("HAX_NO_AGENTS_MD");
    sb_free(&s);
}

static void test_xdg_config_home_overrides_home(void)
{
    struct sandbox s;
    sb_init(&s);
    /* Two candidate global locations: HOME-based one (via sb_init) and an
     * explicit XDG_CONFIG_HOME pointing elsewhere. The XDG one must win. */
    char *home_global = xasprintf("%s/.config/hax/AGENTS.md", s.root);
    write_file(home_global, "HOME_GLOBAL\n");
    free(home_global);

    char *xdg_dir = xasprintf("%s/xdg/hax", s.root);
    mkdirs(xdg_dir);
    char *xdg_md = xasprintf("%s/AGENTS.md", xdg_dir);
    write_file(xdg_md, "XDG_GLOBAL\n");
    free(xdg_md);
    char *xdg_root = xasprintf("%s/xdg", s.root);
    setenv("XDG_CONFIG_HOME", xdg_root, 1);
    free(xdg_root);
    free(xdg_dir);

    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    setenv("HAX_NO_ENV", "1", 1);
    char *p = env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "XDG_GLOBAL"));
        EXPECT(!contains(p, "HOME_GLOBAL"));
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    unsetenv("XDG_CONFIG_HOME");
    sb_free(&s);
}

static void test_agents_md_invalid_bytes_sanitized(void)
{
    struct sandbox s;
    sb_init(&s);
    /* AGENTS.md with an embedded NUL and an invalid UTF-8 byte. The raw
     * bytes would truncate the prompt under strlen and Jansson would
     * reject the request as non-UTF-8 — sanitize_utf8 must replace both
     * with U+FFFD before they enter the buffer. */
    char *git = xasprintf("%s/.git", s.root);
    mkdir(git, 0755);
    free(git);
    char *md = xasprintf("%s/AGENTS.md", s.root);
    const char dirty[] = "before\0middle\xFF"
                         "after\n";
    write_file_bytes(md, dirty, sizeof(dirty) - 1);
    free(md);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    setenv("HAX_NO_ENV", "1", 1);
    char *p = env_build_suffix("m");
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
    sb_free(&s);
}

/* ---------- skills ---------- */

static void test_skills_none(void)
{
    struct sandbox s;
    sb_init(&s);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    setenv("HAX_NO_ENV", "1", 1);
    char *p = env_build_suffix("m");
    /* No AGENTS.md, no skills, env disabled → NULL. */
    EXPECT(p == NULL);
    free(p);
    unsetenv("HAX_NO_ENV");
    sb_free(&s);
}

static void test_skills_with_description_sorted(void)
{
    struct sandbox s;
    sb_init(&s);
    /* Two skills with frontmatter; verify sorted output and that the
     * description field is parsed and emitted. */
    char *zskill = xasprintf("%s/.agents/skills/zeta/SKILL.md", s.root);
    write_file(zskill, "---\nname: zeta\ndescription: zeta does Z\n---\n# Zeta\n\nbody\n");
    free(zskill);
    char *askill = xasprintf("%s/.agents/skills/alpha/SKILL.md", s.root);
    write_file(askill, "---\nname: alpha\ndescription: \"alpha does A\"\n---\nbody\n");
    free(askill);

    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    setenv("HAX_NO_ENV", "1", 1);
    char *p = env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "# Skills"));
        EXPECT(contains(p, "- alpha: alpha does A (.agents/skills/alpha/SKILL.md)"));
        EXPECT(contains(p, "- zeta: zeta does Z (.agents/skills/zeta/SKILL.md)"));
        const char *a = strstr(p, "- alpha");
        const char *z = strstr(p, "- zeta");
        EXPECT(a && z && a < z);
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sb_free(&s);
}

static void test_skills_crlf_frontmatter(void)
{
    struct sandbox s;
    sb_init(&s);
    /* Files checked out on Windows-style line endings have CRLF
     * everywhere, including the opening `---` fence. The closer already
     * accepts \r — verify the opener does too, otherwise the description
     * silently goes missing for these files. */
    char *path = xasprintf("%s/.agents/skills/crlf/SKILL.md", s.root);
    const char body[] = "---\r\ndescription: from crlf\r\n---\r\nbody\r\n";
    write_file_bytes(path, body, sizeof(body) - 1);
    free(path);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    setenv("HAX_NO_ENV", "1", 1);
    char *p = env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "- crlf: from crlf (.agents/skills/crlf/SKILL.md)"));
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sb_free(&s);
}

static void test_skills_no_frontmatter_falls_back_to_dir(void)
{
    struct sandbox s;
    sb_init(&s);
    char *path = xasprintf("%s/.agents/skills/raw/SKILL.md", s.root);
    write_file(path, "Just a body, no frontmatter at all.\n");
    free(path);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    setenv("HAX_NO_ENV", "1", 1);
    char *p = env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "- raw (.agents/skills/raw/SKILL.md)"));
        EXPECT(!contains(p, "raw:")); /* no description → no colon-and-text */
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sb_free(&s);
}

static void test_skills_dir_without_skill_md_skipped(void)
{
    struct sandbox s;
    sb_init(&s);
    /* Subdir exists but has no SKILL.md inside — must be skipped. */
    char *dir = xasprintf("%s/.agents/skills/empty", s.root);
    mkdirs(dir);
    free(dir);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    setenv("HAX_NO_ENV", "1", 1);
    char *p = env_build_suffix("m");
    /* Nothing valid → NULL. */
    EXPECT(p == NULL);
    free(p);
    unsetenv("HAX_NO_ENV");
    sb_free(&s);
}

static void test_skills_global_root(void)
{
    struct sandbox s;
    sb_init(&s);
    /* Global skill via $HOME/.config/hax/skills (HOME is sandboxed). */
    char *gpath = xasprintf("%s/.config/hax/skills/sample/SKILL.md", s.root);
    write_file(gpath, "---\ndescription: from global\n---\n");
    free(gpath);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    setenv("HAX_NO_ENV", "1", 1);
    char *p = env_build_suffix("m");
    EXPECT(p != NULL);
    if (p) {
        EXPECT(contains(p, "- sample: from global"));
        /* Global path is absolute, embedded under the sandbox root. */
        EXPECT(contains(p, "/.config/hax/skills/sample/SKILL.md"));
        free(p);
    }
    unsetenv("HAX_NO_ENV");
    sb_free(&s);
}

static void test_skills_project_shadows_global(void)
{
    struct sandbox s;
    sb_init(&s);
    char *gpath = xasprintf("%s/.config/hax/skills/dup/SKILL.md", s.root);
    write_file(gpath, "---\ndescription: from global\n---\n");
    free(gpath);
    char *ppath = xasprintf("%s/.agents/skills/dup/SKILL.md", s.root);
    write_file(ppath, "---\ndescription: from project\n---\n");
    free(ppath);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    setenv("HAX_NO_ENV", "1", 1);
    char *p = env_build_suffix("m");
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
    sb_free(&s);
}

static void test_skills_disabled_by_no_agents_md(void)
{
    struct sandbox s;
    sb_init(&s);
    char *path = xasprintf("%s/.agents/skills/foo/SKILL.md", s.root);
    write_file(path, "---\ndescription: hidden\n---\n");
    free(path);
    if (chdir(s.root) != 0) {
        sb_free(&s);
        return;
    }
    setenv("HAX_NO_AGENTS_MD", "1", 1);
    char *p = env_build_suffix("m");
    EXPECT(p != NULL); /* env block still present */
    if (p) {
        EXPECT(!contains(p, "# Skills"));
        EXPECT(!contains(p, "hidden"));
        free(p);
    }
    unsetenv("HAX_NO_AGENTS_MD");
    sb_free(&s);
}

int main(void)
{
    test_env_block_present_by_default();
    test_env_block_git_repo_yes();
    test_env_block_git_repo_yes_from_subdir();
    test_env_block_omits_model_when_null();
    test_no_env_knob_disables_block();
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
    test_skills_disabled_by_no_agents_md();

    T_REPORT();
}
