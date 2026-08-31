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
#include "xalloc.h"
#include "system/fs.h"

/* HOME, XDG paths, cwd, and prompt feature flags are isolated from the developer's environment. */
struct sandbox {
    char *root;
    char *previous_cwd;
};

static void sandbox_init(struct sandbox *sandbox)
{
    sandbox->previous_cwd = getcwd(NULL, 0);
    sandbox->root = xstrdup(t_tempdir());
    setenv("HOME", sandbox->root, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
    unsetenv("HAX_NO_SKILLS");
    setenv("HAX_BASH_SHELL", CONFIG_VALUE_DEFAULT, 1);

    /* Most tests exercise one context source; delegation guidance is covered separately. */
    setenv("HAX_NO_SUBAGENTS", "1", 1);
    setenv("HAX_NO_TASKS", "1", 1);
}

static void sandbox_free(struct sandbox *sandbox)
{
    if (sandbox->previous_cwd) {
        if (chdir(sandbox->previous_cwd) != 0)
            FAIL("chdir(previous_cwd=%s): %s", sandbox->previous_cwd, strerror(errno));
        free(sandbox->previous_cwd);
    }
    free(sandbox->root);
}

static void write_file_bytes(const char *path, const void *data, size_t data_len)
{
    char *parent = xstrdup(path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        if (fs_mkdir_p(parent) != 0)
            FAIL("mkdir(%s): %s", parent, strerror(errno));
    }
    free(parent);
    FILE *f = fopen(path, "w");
    if (!f) {
        FAIL("fopen(%s): %s", path, strerror(errno));
        return;
    }
    if (data_len && fwrite(data, 1, data_len, f) != data_len)
        FAIL("short write to %s", path);
    fclose(f);
}

static void write_file(const char *path, const char *content)
{
    write_file_bytes(path, content, strlen(content));
}

static int contains(const char *haystack, const char *needle)
{
    return haystack && strstr(haystack, needle) != NULL;
}

/* ---------- sandbox-relative staging ---------- */

static char *sandbox_path(struct sandbox *sandbox, const char *relative_path)
{
    if (!relative_path || !*relative_path || strcmp(relative_path, ".") == 0)
        return xstrdup(sandbox->root);
    return xasprintf("%s/%s", sandbox->root, relative_path);
}

static void sandbox_mkdir(struct sandbox *sandbox, const char *relative_path)
{
    char *dir = sandbox_path(sandbox, relative_path);
    if (fs_mkdir_p(dir) != 0)
        FAIL("mkdir(%s): %s", dir, strerror(errno));
    free(dir);
}

static void sandbox_write(struct sandbox *sandbox, const char *relative_path, const char *content)
{
    char *path = sandbox_path(sandbox, relative_path);
    write_file(path, content);
    free(path);
}

static void sandbox_write_bytes(struct sandbox *sandbox, const char *relative_path,
                                const void *data, size_t data_len)
{
    char *path = sandbox_path(sandbox, relative_path);
    write_file_bytes(path, data, data_len);
    free(path);
}

/* fs_which() only needs the staged command to be a regular executable file. */
static void sandbox_stage_command(struct sandbox *sandbox, const char *relative_dir,
                                  const char *name)
{
    sandbox_mkdir(sandbox, relative_dir);
    char *dir = sandbox_path(sandbox, relative_dir);
    char *path = xasprintf("%s/%s", dir, name);
    free(dir);
    write_file(path, "#!/bin/sh\n");
    if (chmod(path, 0755) != 0)
        FAIL("chmod(%s): %s", path, strerror(errno));
    free(path);
}

static int sandbox_chdir(struct sandbox *sandbox, const char *relative_path)
{
    char *dir = sandbox_path(sandbox, relative_path);
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

static void test_environment_present_by_default(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    char *suffix = agent_env_build_suffix("claude-test-1");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "# Environment\n\n"));
        EXPECT(contains(suffix, "- Working directory: ~\n"));
        char *home = xasprintf("- Home directory: %s\n", s.root);
        EXPECT(contains(suffix, home));
        free(home);
        EXPECT(contains(suffix, "- Operating system: "));
        EXPECT(contains(suffix, "- Command shell: "));
        EXPECT(contains(suffix, "- Model: claude-test-1\n"));
        EXPECT(contains(suffix, "- Git repository: no\n"));
        EXPECT(!contains(suffix, "<env>"));
        free(suffix);
    }
    sandbox_free(&s);
}

static void test_environment_reports_git_root(void)
{
    struct sandbox s;
    sandbox_init(&s);
    sandbox_mkdir(&s, ".git");
    SANDBOX_CHDIR(&s, ".");
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "- Git repository root: ~\n"));
        free(suffix);
    }
    sandbox_free(&s);
}

static void test_environment_finds_git_root_from_subdir(void)
{
    struct sandbox s;
    sandbox_init(&s);
    /* .git lives at the sandbox root; cwd is two levels deeper. The
     * Environment section should report that root using the same upward walk
     * as AGENTS.md. */
    sandbox_mkdir(&s, ".git");
    sandbox_mkdir(&s, "a/b");
    SANDBOX_CHDIR(&s, "a/b");
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "- Git repository root: ~\n"));
        free(suffix);
    }
    sandbox_free(&s);
}

static void test_environment_omits_null_model(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    char *suffix = agent_env_build_suffix(NULL);
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(!contains(suffix, "- Model:"));
        free(suffix);
    }
    sandbox_free(&s);
}

static void test_environment_omits_unset_home(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    unsetenv("HOME");
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(!contains(suffix, "- Home directory:"));
        char *cwd = xasprintf("- Working directory: %s\n", s.root);
        EXPECT(contains(suffix, cwd));
        free(cwd);
        free(suffix);
    }
    sandbox_free(&s);
}

static void test_environment_reports_command_shell(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_BASH_SHELL", "/bin/sh", 1);
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "- Command shell: /bin/sh\n"));
        free(suffix);
    }
    sandbox_free(&s);
}

static void test_environment_can_be_disabled(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix == NULL);
    free(suffix);
    sandbox_free(&s);
}

static void test_all_context_sections_can_be_disabled(void)
{
    struct sandbox s;
    sandbox_init(&s);
    sandbox_write(&s, "AGENTS.md", "project guidance\n");
    sandbox_write(&s, ".agents/skills/example/SKILL.md", "---\ndescription: example\n---\n");
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);
    setenv("HAX_NO_AGENTS_MD", "1", 1);
    setenv("HAX_NO_SKILLS", "1", 1);

    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix == NULL);
    free(suffix);
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

    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "Available command-line tools: `rg`, `jq`.\n"));
        EXPECT(contains(suffix, "Prefer `rg` to `grep -r`.\n"));
        free(suffix);
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

    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(!contains(suffix, "Available command-line tools:"));
        EXPECT(!contains(suffix, "Prefer `"));
        free(suffix);
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

    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(!contains(suffix, "Available command-line tools:"));
        EXPECT(!contains(suffix, "Prefer `"));
        EXPECT(!contains(suffix, "`rg`"));
        free(suffix);
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

    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "Available command-line tools: `jq`.\n"));
        EXPECT(!contains(suffix, "Prefer `"));
        EXPECT(!contains(suffix, "`rg`"));
        free(suffix);
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

    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix,
                        "Available command-line tools: `rg`, `fd`, `gh`, `python3`, `node`.\n"));
        EXPECT(contains(suffix, "Prefer `rg` to `grep -r`, `fd` to `find`, `python3` to "
                                "`python`.\n"));
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    /* Without .git anywhere, the parent file is ignored and there is no
     * cwd-level file → nothing to emit, suffix is NULL. */
    EXPECT(suffix == NULL);
    free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        const char *outer = strstr(suffix, "OUTER_MARKER");
        const char *middle = strstr(suffix, "MIDDLE_MARKER");
        const char *inner = strstr(suffix, "INNER_MARKER");
        EXPECT(outer && middle && inner);
        if (outer && middle && inner) {
            EXPECT(outer < middle);
            EXPECT(middle < inner);
        }
        EXPECT(contains(suffix, "# Project Context"));
        EXPECT(contains(suffix, "Project guidance below overrides the assistant defaults above."));
        EXPECT(contains(suffix, "## "));
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        const char *global = strstr(suffix, "GLOBAL_MARKER");
        const char *local = strstr(suffix, "LOCAL_MARKER");
        EXPECT(global && local);
        if (global && local)
            EXPECT(global < local);
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(!contains(suffix, "SHOULD_NOT_APPEAR"));
        EXPECT(!contains(suffix, "# Project Context"));
        EXPECT(contains(suffix, "# Environment"));
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "XDG_GLOBAL"));
        EXPECT(!contains(suffix, "HOME_GLOBAL"));
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        /* Both `before` (pre-NUL) and `after` (post-replacement) survive,
         * which proves the prompt didn't get truncated mid-file. */
        EXPECT(contains(suffix, "before"));
        EXPECT(contains(suffix, "middle"));
        EXPECT(contains(suffix, "after"));
        /* No raw NUL anywhere in the C string (strlen would already cut
         * the buffer at one) and no raw 0xFF byte. */
        EXPECT(strlen(suffix) > strlen("before") + strlen("middle") + strlen("after"));
        EXPECT(memchr(suffix, '\xFF', strlen(suffix)) == NULL);
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    /* No AGENTS.md, no skills, env disabled → NULL. */
    EXPECT(suffix == NULL);
    free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "# Skills"));
        /* sandbox_init pins HOME=sandbox root, so absolute project paths collapse
         * to `~/.agents/skills/...`. */
        EXPECT(contains(suffix, "- alpha: alpha does A (~/.agents/skills/alpha/SKILL.md)"));
        EXPECT(contains(suffix, "- zeta: zeta does Z (~/.agents/skills/zeta/SKILL.md)"));
        const char *alpha = strstr(suffix, "- alpha");
        const char *zeta = strstr(suffix, "- zeta");
        EXPECT(alpha && zeta && alpha < zeta);
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "- crlf: from crlf (~/.agents/skills/crlf/SKILL.md)"));
        free(suffix);
    }
    unsetenv("HAX_NO_ENV");
    sandbox_free(&s);
}

static void test_skills_unterminated_frontmatter_omits_description(void)
{
    struct sandbox s;
    sandbox_init(&s);
    sandbox_write(&s, ".agents/skills/broken/SKILL.md", "---\ndescription: incomplete\n");
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);

    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "- broken (~/.agents/skills/broken/SKILL.md)"));
        EXPECT(!contains(suffix, "- broken:"));
        free(suffix);
    }
    sandbox_free(&s);
}

static void test_skills_block_description_falls_back_to_name(void)
{
    struct sandbox s;
    sandbox_init(&s);
    sandbox_write(&s, ".agents/skills/block/SKILL.md",
                  "---\ndescription: |-\n  multiline description\n---\n");
    sandbox_write(&s, ".agents/skills/literal/SKILL.md", "---\ndescription: \"|\"\n---\n");
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);

    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "- block (~/.agents/skills/block/SKILL.md)"));
        EXPECT(!contains(suffix, "- block:"));
        EXPECT(contains(suffix, "- literal: | (~/.agents/skills/literal/SKILL.md)"));
        free(suffix);
    }
    sandbox_free(&s);
}

static void test_skills_no_frontmatter_falls_back_to_dir(void)
{
    struct sandbox s;
    sandbox_init(&s);
    sandbox_write(&s, ".agents/skills/raw/SKILL.md", "Just a body, no frontmatter at all.\n");
    SANDBOX_CHDIR(&s, ".");
    setenv("HAX_NO_ENV", "1", 1);
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "- raw (~/.agents/skills/raw/SKILL.md)"));
        EXPECT(!contains(suffix, "raw:")); /* no description → no colon-and-text */
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    /* Nothing valid → NULL. */
    EXPECT(suffix == NULL);
    free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "- sample: from global"));
        /* Global path is absolute, embedded under the sandbox root. */
        EXPECT(contains(suffix, "/.config/hax/skills/sample/SKILL.md"));
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "from project"));
        EXPECT(!contains(suffix, "from global"));
        /* And exactly one entry for `dup`. */
        const char *first = strstr(suffix, "- dup");
        EXPECT(first != NULL);
        if (first)
            EXPECT(strstr(first + 1, "- dup") == NULL);
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL); /* Environment section still present */
    if (suffix) {
        EXPECT(!contains(suffix, "# Skills"));
        EXPECT(!contains(suffix, "hidden"));
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "- rooted: from project root"));
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "from subdir"));
        EXPECT(!contains(suffix, "from root"));
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix == NULL);
    free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "- shared: from shared"));
        EXPECT(contains(suffix, "~/.agents/skills/shared/SKILL.md"));
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "from hax global"));
        EXPECT(!contains(suffix, "from shared"));
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "from hax global"));
        EXPECT(!contains(suffix, "from shared"));
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "from hax global"));
        EXPECT(!contains(suffix, "from shared"));
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "- only: from shared"));
        free(suffix);
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
    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(!contains(suffix, "project rules"));
        EXPECT(contains(suffix, "# Skills"));
        EXPECT(contains(suffix, "still here"));
        free(suffix);
    }
    unsetenv("HAX_NO_AGENTS_MD");
    sandbox_free(&s);
}

/* ---------- subagents section ---------- */

static void test_subagents_follow_task_guidance(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    unsetenv("HAX_NO_SUBAGENTS");
    unsetenv("HAX_NO_TASKS");

    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "# Subagents"));
        EXPECT(contains(suffix, "only when the user asks"));
        EXPECT(contains(suffix, "task_wait"));
        EXPECT(!contains(suffix, "--preset"));
        const char *background_tasks = strstr(suffix, "# Background tasks");
        const char *subagents = strstr(suffix, "# Subagents");
        const char *environment = strstr(suffix, "# Environment");
        EXPECT(background_tasks && subagents && environment && background_tasks < subagents &&
               subagents < environment);
        free(suffix);
    }
    sandbox_free(&s);
}

static void test_subagents_without_background_tasks(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    unsetenv("HAX_NO_SUBAGENTS");

    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(!contains(suffix, "# Background tasks"));
        EXPECT(contains(suffix, "# Subagents"));
        EXPECT(contains(suffix, "timeout_seconds (e.g. 1800)"));
        EXPECT(!contains(suffix, "task_wait"));
        free(suffix);
    }
    sandbox_free(&s);
}

static void test_subagent_presets_are_filtered_and_sorted(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    unsetenv("HAX_NO_SUBAGENTS");
    EXPECT(config_load("{\"presets\": {"
                       "\"zeta\": {\"provider\": \"mock\", \"model\": \"m2\"},"
                       "\"typo\": {\"provider\": \"does-not-exist\", "
                       "\"description\": \"broken role\"},"
                       "\"review\": {\"provider\": \"mock\", "
                       "\"description\": \"code review stance\"},"
                       "\"alpha\": {\"provider\": \"mock\", "
                       "\"description\": \"quick answers\"}}}") == 0);

    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(contains(suffix, "Presets (select with `--preset <name>`):\n"
                                "- alpha: quick answers\n- review: code review stance\n"));
        EXPECT(!contains(suffix, "zeta"));
        EXPECT(!contains(suffix, "typo"));
        free(suffix);
    }
    config_load(NULL);
    sandbox_free(&s);
}

static void test_subagent_preset_heading_requires_valid_role(void)
{
    struct sandbox s;
    sandbox_init(&s);
    SANDBOX_CHDIR(&s, ".");
    unsetenv("HAX_NO_SUBAGENTS");
    EXPECT(config_load("{\"presets\": {"
                       "\"a\": {\"provider\": \"does-not-exist\", "
                       "\"description\": \"broken\"},"
                       "\"b\": {\"provider\": \"mock\"}}}") == 0);

    char *suffix = agent_env_build_suffix("m");
    EXPECT(suffix != NULL);
    if (suffix) {
        EXPECT(!contains(suffix, "Presets ("));
        EXPECT(!contains(suffix, "--preset"));
        free(suffix);
    }
    config_load(NULL);
    sandbox_free(&s);
}

int main(void)
{
    test_environment_present_by_default();
    test_environment_reports_git_root();
    test_environment_finds_git_root_from_subdir();
    test_environment_omits_null_model();
    test_environment_omits_unset_home();
    test_environment_reports_command_shell();
    test_environment_can_be_disabled();
    test_all_context_sections_can_be_disabled();

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
    test_skills_unterminated_frontmatter_omits_description();
    test_skills_block_description_falls_back_to_name();
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

    test_subagents_follow_task_guidance();
    test_subagents_without_background_tasks();
    test_subagent_presets_are_filtered_and_sorted();
    test_subagent_preset_heading_requires_valid_role();

    T_REPORT();
}
