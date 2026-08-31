/* SPDX-License-Identifier: MIT */
#include "agent_env.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "buf.h"
#include "config.h"
#include "diag.h"
#include "xalloc.h"
#include "providers/registry.h"
#include "system/fs.h"
#include "system/os.h"
#include "system/path.h"
#include "text/utf8_sanitize.h"
#include "tools/bash_shell.h"

/* Truncate oversized project instructions rather than letting one file dominate the prompt. */
#define AGENTS_MD_MAX_BYTES (64u * 1024u)

/* Bound upward discovery even on an unusually deep tree without a project marker. */
#define PROJECT_MAX_DEPTH 64

/* Skill discovery needs only YAML metadata, never the full SKILL.md body. */
#define SKILL_FRONTMATTER_MAX_BYTES 8192

/* Clamp descriptions to the skill metadata specification's limit. */
#define SKILL_DESCRIPTION_MAX_BYTES 1024

/* Probe project-independent tools only; project tooling is inferred from project files. */
struct command_probe {
    const char *name;
    const char *replacement_for;
};

static const struct command_probe COMMAND_PROBES[] = {
    {"rg", "grep -r"},     {"fd", "find"}, {"jq", NULL},     {"gh", NULL},
    {"python3", "python"}, {"node", NULL}, {"magick", NULL},
};
static const size_t COMMAND_PROBE_COUNT = sizeof(COMMAND_PROBES) / sizeof(COMMAND_PROBES[0]);

static int command_is_available(const char *name)
{
    char *path = fs_which(name);
    if (!path)
        return 0;
    free(path);
    return 1;
}

/* Replace absolute `dir` with its parent; return 0 at the root without changing it. */
static int climb_to_parent(char *dir)
{
    char *slash = strrchr(dir, '/');
    if (!slash)
        return 0;
    if (slash == dir) {
        if (dir[1] == '\0')
            return 0;
        dir[1] = '\0';
        return 1;
    }
    *slash = '\0';
    return 1;
}

static char *find_project_root(const char *cwd)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", cwd);
    for (int depth = 0; depth < PROJECT_MAX_DEPTH; depth++) {
        char marker[PATH_MAX + 16];
        snprintf(marker, sizeof(marker), "%s/.git", dir);
        struct stat marker_stat;
        if (stat(marker, &marker_stat) == 0)
            return xstrdup(dir);

        if (!climb_to_parent(dir))
            break;
    }
    return NULL;
}

static char *sanitize_display_path(const char *path)
{
    /* Collapse before sanitizing so the raw $HOME prefix can still match. */
    char *display_path = path_collapse_home(path);
    char *clean_path = utf8_sanitize(display_path, strlen(display_path));
    free(display_path);
    return clean_path;
}

static void append_environment_value(struct buf *prompt, const char *label, const char *value)
{
    char *line = xasprintf("- %s: %s\n", label, value);
    buf_append_str(prompt, line);
    free(line);
}

static void append_command_summary(struct buf *prompt)
{
    int available[sizeof(COMMAND_PROBES) / sizeof(COMMAND_PROBES[0])];
    for (size_t i = 0; i < COMMAND_PROBE_COUNT; i++)
        available[i] = command_is_available(COMMAND_PROBES[i].name);

    int has_commands = 0;
    for (size_t i = 0; i < COMMAND_PROBE_COUNT; i++) {
        if (!available[i])
            continue;
        buf_append_str(prompt, has_commands ? ", `" : "\nAvailable command-line tools: `");
        buf_append_str(prompt, COMMAND_PROBES[i].name);
        buf_append_str(prompt, "`");
        has_commands = 1;
    }
    if (has_commands)
        buf_append_str(prompt, ".\n");

    int has_replacements = 0;
    for (size_t i = 0; i < COMMAND_PROBE_COUNT; i++) {
        if (!available[i] || !COMMAND_PROBES[i].replacement_for)
            continue;
        buf_append_str(prompt, has_replacements ? ", " : "Prefer ");
        char *guidance =
            xasprintf("`%s` to `%s`", COMMAND_PROBES[i].name, COMMAND_PROBES[i].replacement_for);
        buf_append_str(prompt, guidance);
        free(guidance);
        has_replacements = 1;
    }
    if (has_replacements)
        buf_append_str(prompt, ".\n");
}

static void append_environment(struct buf *prompt, const char *model)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        snprintf(cwd, sizeof(cwd), "(unknown)");

    const char *home = getenv("HOME");
    char *shell = bash_resolve_shell();
    char *os = os_description();
    char *project_root = find_project_root(cwd);

    char *cwd_clean = sanitize_display_path(cwd);
    char *home_clean = (home && *home) ? utf8_sanitize(home, strlen(home)) : NULL;
    char *os_clean = utf8_sanitize(os, strlen(os));
    char *shell_clean = utf8_sanitize(shell, strlen(shell));
    char *model_clean = (model && *model) ? utf8_sanitize(model, strlen(model)) : NULL;
    char *root_clean = project_root ? sanitize_display_path(project_root) : NULL;

    if (prompt->len > 0)
        buf_append_str(prompt, "\n");
    buf_append_str(prompt, "# Environment\n\n");
    append_environment_value(prompt, "Working directory", cwd_clean);
    if (home_clean)
        append_environment_value(prompt, "Home directory", home_clean);
    append_environment_value(prompt, "Operating system", os_clean);
    append_environment_value(prompt, "Command shell", shell_clean);
    if (model_clean)
        append_environment_value(prompt, "Model", model_clean);
    if (root_clean)
        append_environment_value(prompt, "Git repository root", root_clean);
    else
        buf_append_str(prompt, "- Git repository: no\n");
    append_command_summary(prompt);

    free(cwd_clean);
    free(home_clean);
    free(os_clean);
    free(shell_clean);
    free(model_clean);
    free(root_clean);
    free(shell);
    free(os);
    free(project_root);
}

static void append_agents_file(struct buf *prompt, const char *path, const char *display_path,
                               int *has_project_context)
{
    size_t content_len = 0;
    int truncated = 0;
    char *content = fs_read_file_capped(path, AGENTS_MD_MAX_BYTES, &content_len, &truncated);
    if (!content)
        return;

    /* Provider JSON requires NUL-free UTF-8, but paths and file contents are arbitrary bytes. */
    char *clean_content = utf8_sanitize(content, content_len);
    free(content);
    size_t clean_len = strlen(clean_content);
    const char *heading_path = display_path ? display_path : path;
    char *clean_path = utf8_sanitize(heading_path, strlen(heading_path));

    if (!*has_project_context) {
        if (prompt->len > 0)
            buf_append_str(prompt, "\n");
        buf_append_str(prompt, "# Project Context\n\n"
                               "Project guidance below overrides the assistant defaults above.\n");
        *has_project_context = 1;
    }
    buf_append_str(prompt, "\n## ");
    buf_append_str(prompt, clean_path);
    buf_append_str(prompt, "\n\n");
    buf_append(prompt, clean_content, clean_len);
    if (clean_len == 0 || clean_content[clean_len - 1] != '\n')
        buf_append_str(prompt, "\n");
    if (truncated)
        buf_append_str(prompt, "[truncated]\n");

    free(clean_content);
    free(clean_path);
}

static void append_project_agents_files(struct buf *prompt, int *has_project_context)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        return;

    char *project_root = find_project_root(cwd);
    if (!project_root) {
        /* Outside a repository, parent instructions may belong to an unrelated project. */
        char *path = path_join(cwd, "AGENTS.md");
        char *display_path = path_collapse_home(path);
        append_agents_file(prompt, path, display_path, has_project_context);
        free(display_path);
        free(path);
        return;
    }

    /* Absolute headings remain reusable by the model; root-first order gives nearer files
     * precedence. */
    char *paths[PROJECT_MAX_DEPTH];
    char *display_paths[PROJECT_MAX_DEPTH];
    size_t path_count = 0;
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", cwd);
    for (int depth = 0; depth < PROJECT_MAX_DEPTH; depth++) {
        paths[path_count] = path_join(dir, "AGENTS.md");
        display_paths[path_count] = path_collapse_home(paths[path_count]);
        path_count++;

        if (strcmp(dir, project_root) == 0 || !climb_to_parent(dir))
            break;
    }
    free(project_root);

    while (path_count > 0) {
        size_t index = --path_count;
        append_agents_file(prompt, paths[index], display_paths[index], has_project_context);
        free(paths[index]);
        free(display_paths[index]);
    }
}

/* Parse one-line, optionally quoted YAML descriptions. Return metadata only after a closing fence
 * confirms that the frontmatter is complete. */
static char *parse_skill_description(const char *content, size_t content_len)
{
    const char *cursor;
    if (content_len >= 4 && memcmp(content, "---\n", 4) == 0)
        cursor = content + 4;
    else if (content_len >= 5 && memcmp(content, "---\r\n", 5) == 0)
        cursor = content + 5;
    else
        return NULL;

    char *description = NULL;
    const char *content_end = content + content_len;
    while (cursor < content_end) {
        const char *line_end = memchr(cursor, '\n', content_end - cursor);
        if (!line_end)
            line_end = content_end;
        size_t line_len = line_end - cursor;

        if ((line_len == 3 && memcmp(cursor, "---", 3) == 0) ||
            (line_len == 4 && memcmp(cursor, "---\r", 4) == 0))
            return description;

        if (!description && line_len > 12 && memcmp(cursor, "description:", 12) == 0) {
            const char *value = cursor + 12;
            const char *value_end = line_end;
            while (value < value_end && (*value == ' ' || *value == '\t'))
                value++;
            while (value_end > value &&
                   (value_end[-1] == ' ' || value_end[-1] == '\t' || value_end[-1] == '\r'))
                value_end--;
            int quoted = value_end - value >= 2 && ((*value == '"' && value_end[-1] == '"') ||
                                                    (*value == '\'' && value_end[-1] == '\''));
            if (quoted) {
                value++;
                value_end--;
            }
            size_t value_len = value_end > value ? (size_t)(value_end - value) : 0;
            if (!quoted && value_len > 0 && (*value == '|' || *value == '>'))
                value_len = 0;
            if (value_len > SKILL_DESCRIPTION_MAX_BYTES)
                value_len = SKILL_DESCRIPTION_MAX_BYTES;
            if (value_len > 0)
                description = utf8_sanitize(value, value_len);
        }
        if (line_end == content_end)
            break;
        cursor = line_end + 1;
    }

    free(description);
    return NULL;
}

struct skill_entry {
    char *name;
    char *display_path;
    char *description;
};

static int compare_skills(const void *a, const void *b)
{
    const struct skill_entry *left = a;
    const struct skill_entry *right = b;
    return strcmp(left->name, right->name);
}

struct skill_list {
    struct skill_entry *entries;
    size_t count;
    size_t capacity;
};

static int skill_list_contains(const struct skill_list *skills, const char *name)
{
    for (size_t i = 0; i < skills->count; i++) {
        if (strcmp(skills->entries[i].name, name) == 0)
            return 1;
    }
    return 0;
}

static void skill_list_add(struct skill_list *skills, struct skill_entry entry)
{
    if (skills->count == skills->capacity) {
        size_t capacity = skills->capacity ? skills->capacity * 2 : 8;
        skills->entries = xrealloc(skills->entries, capacity * sizeof(*skills->entries));
        skills->capacity = capacity;
    }
    skills->entries[skills->count++] = entry;
}

static void skill_list_free(struct skill_list *skills)
{
    for (size_t i = 0; i < skills->count; i++) {
        free(skills->entries[i].name);
        free(skills->entries[i].display_path);
        free(skills->entries[i].description);
    }
    free(skills->entries);
}

/* Earlier roots win when duplicate skill names are found. */
static void collect_skills(struct skill_list *skills, const char *root)
{
    DIR *dir = opendir(root);
    if (!dir)
        return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        /* Keep raw filesystem bytes for I/O, but compare and display sanitized names. */
        char *name = utf8_sanitize(entry->d_name, strlen(entry->d_name));
        if (skill_list_contains(skills, name)) {
            free(name);
            continue;
        }

        char *skill_dir = path_join(root, entry->d_name);
        char *skill_path = path_join(skill_dir, "SKILL.md");
        free(skill_dir);
        size_t frontmatter_len = 0;
        char *frontmatter =
            fs_read_file_capped(skill_path, SKILL_FRONTMATTER_MAX_BYTES, &frontmatter_len, NULL);
        if (!frontmatter) {
            free(name);
            free(skill_path);
            continue;
        }

        struct skill_entry skill = {
            .name = name,
            .display_path = sanitize_display_path(skill_path),
            .description = parse_skill_description(frontmatter, frontmatter_len),
        };
        skill_list_add(skills, skill);
        free(frontmatter);
        free(skill_path);
    }
    closedir(dir);
}

/* Walk nearest-first so closer project skills shadow ancestors; without a Git root, inspect only
 * cwd. */
static void collect_project_skills(struct skill_list *skills, const char *excluded_root)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        return;

    /* Device and inode identify a symlinked $HOME root when getcwd() uses its physical path. */
    struct stat excluded_stat;
    int have_excluded_root = excluded_root && stat(excluded_root, &excluded_stat) == 0;

    char *project_root = find_project_root(cwd);
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", cwd);
    for (int depth = 0; depth < PROJECT_MAX_DEPTH; depth++) {
        char *skills_dir = path_join(dir, ".agents/skills");
        struct stat skills_stat;
        int is_excluded = have_excluded_root && stat(skills_dir, &skills_stat) == 0 &&
                          skills_stat.st_dev == excluded_stat.st_dev &&
                          skills_stat.st_ino == excluded_stat.st_ino;
        if (!is_excluded)
            collect_skills(skills, skills_dir);
        free(skills_dir);

        if (!project_root || strcmp(dir, project_root) == 0 || !climb_to_parent(dir))
            break;
    }
    free(project_root);
}

static void append_skills(struct buf *prompt)
{
    struct skill_list skills = {0};
    const char *home = getenv("HOME");
    char *shared_root = (home && *home) ? path_join(home, ".agents/skills") : NULL;

    /* ~/.agents/skills is shared across agents. Hold it out of the project walk so cwd under $HOME
     * cannot move it ahead of hax's XDG skill root. */
    collect_project_skills(&skills, shared_root);

    char *hax_root = xdg_hax_config_path("skills");
    if (hax_root) {
        collect_skills(&skills, hax_root);
        free(hax_root);
    }
    if (shared_root) {
        collect_skills(&skills, shared_root);
        free(shared_root);
    }

    if (skills.count == 0)
        return;

    qsort(skills.entries, skills.count, sizeof(*skills.entries), compare_skills);
    if (prompt->len > 0)
        buf_append_str(prompt, "\n");
    buf_append_str(prompt,
                   "# Skills\n\n"
                   "Read the corresponding SKILL.md when a task matches the description:\n\n");
    for (size_t i = 0; i < skills.count; i++) {
        const struct skill_entry *skill = &skills.entries[i];
        char *line;
        if (skill->description)
            line =
                xasprintf("- %s: %s (%s)\n", skill->name, skill->description, skill->display_path);
        else
            line = xasprintf("- %s (%s)\n", skill->name, skill->display_path);
        buf_append_str(prompt, line);
        free(line);
    }
    skill_list_free(&skills);
}

/* Presets are the only advertised selector because their names and roles are user-defined. */
static const char SUBAGENTS_PROMPT[] =
    "# Subagents\n"
    "\n"
    "`hax -p \"<task>\"` (via the bash tool) runs a fresh hax instance with clean context in "
    "this directory and prints its final answer to stdout. Delegate to subagents only when "
    "the user asks for it. The child inherits this session's provider, model, and effort. "
    "Launch each subagent with `background: true` and collect answers with task_wait — that "
    "is also how several run in parallel. The child prints its session id to stderr at "
    "startup (captured in the task log); follow up on a finished (or killed) run with "
    "`hax --resume=<id> -p \"<follow-up>\"`.\n";

/* Task-less variant: synchronous calls need a wide timeout to survive a slow child. */
static const char SUBAGENTS_PROMPT_NO_TASKS[] =
    "# Subagents\n"
    "\n"
    "`hax -p \"<task>\"` (via the bash tool) runs a fresh hax instance with clean context in "
    "this directory and prints its final answer to stdout. Delegate to subagents only when "
    "the user asks for it. The child inherits this session's provider, model, and effort. "
    "Subagents are slow: pass a generous timeout_seconds (e.g. 1800). The child prints its "
    "session id to stderr at startup; follow up on a finished (or timed-out) run with "
    "`hax --resume=<id> -p \"<follow-up>\"`.\n";

/* What no single tool description carries: the working loop, the notification contract, and
 * the process-bound lifetime of background tasks. */
static const char TASKS_PROMPT[] =
    "# Background tasks\n"
    "\n"
    "A bash command that outlives its timeout, or is launched with `background: true`, "
    "continues as a background task. Wait on the task whose result you need next with "
    "task_wait — it returns that task's output and status. Completions of other tasks are "
    "announced automatically as one-line notes (with the pending output size); collect an "
    "announced task with task_wait when you want its output. Stop a task with task_wait's "
    "`kill` flag, which also returns its final output. Never pass time with sleep or a "
    "polling loop; give task_wait a timeout instead. "
    "Tasks do not survive the hax process: in a one-shot (-p) run, tasks nobody waited on are "
    "killed once the final answer is produced. The user manages tasks with /tasks.\n";

static void append_tasks(struct buf *prompt)
{
    if (prompt->len > 0)
        buf_append_str(prompt, "\n");
    buf_append_str(prompt, TASKS_PROMPT);
}

static int compare_strings(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void append_subagent_presets(struct buf *prompt)
{
    char **names = NULL;
    size_t name_count = config_preset_names(&names);
    if (name_count > 1)
        qsort(names, name_count, sizeof(*names), compare_strings);

    /* Build the list first so an empty result does not advertise --preset without choices. */
    struct buf list;
    buf_init(&list);
    for (size_t i = 0; i < name_count; i++) {
        /* Descriptions opt favorite presets into being advertised as delegation roles. */
        const char *description = config_preset_description(names[i]);
        if (!description || !*description)
            continue;

        /* Reject registry errors, not transient provider unavailability that may recover. */
        const char *provider = config_preset_provider(names[i]);
        if (!provider || !provider_find(provider)) {
            static int warned;
            if (!warned) {
                warned = 1;
                hax_warn("preset '%s' names unknown provider '%s' — not advertised to the model",
                         names[i], provider ? provider : "?");
            }
            continue;
        }

        char *clean_name = utf8_sanitize(names[i], strlen(names[i]));
        char *clean_description = utf8_sanitize(description, strlen(description));
        char *line = xasprintf("- %s: %s\n", clean_name, clean_description);
        buf_append_str(&list, line);
        free(line);
        free(clean_description);
        free(clean_name);
    }
    if (list.len > 0) {
        buf_append_str(prompt, "\nPresets (select with `--preset <name>`):\n");
        buf_append(prompt, list.data, list.len);
    }

    buf_free(&list);
    for (size_t i = 0; i < name_count; i++)
        free(names[i]);
    free(names);
}

static void append_subagents(struct buf *prompt, int tasks_enabled)
{
    if (prompt->len > 0)
        buf_append_str(prompt, "\n");
    buf_append_str(prompt, tasks_enabled ? SUBAGENTS_PROMPT : SUBAGENTS_PROMPT_NO_TASKS);
    append_subagent_presets(prompt);
}

char *agent_env_build_suffix(const char *model)
{
    int environment_enabled = !config_bool("no_env");
    int agents_md_enabled = !config_bool("no_agents_md");
    int skills_enabled = !config_bool("no_skills");
    int subagents_enabled = !config_bool("no_subagents");
    int tasks_enabled = !config_bool("no_tasks");

    struct buf prompt;
    buf_init(&prompt);

    /* Keep hax guidance ahead of project context; subagent guidance relies on task guidance. */
    if (tasks_enabled)
        append_tasks(&prompt);
    if (subagents_enabled)
        append_subagents(&prompt, tasks_enabled);
    if (environment_enabled)
        append_environment(&prompt, model);

    if (agents_md_enabled) {
        int has_project_context = 0;
        char *global_path = xdg_hax_config_path("AGENTS.md");
        if (global_path) {
            char *display_path = path_collapse_home(global_path);
            append_agents_file(&prompt, global_path, display_path, &has_project_context);
            free(display_path);
            free(global_path);
        }
        append_project_agents_files(&prompt, &has_project_context);
    }

    if (skills_enabled)
        append_skills(&prompt);

    if (prompt.len == 0) {
        buf_free(&prompt);
        return NULL;
    }
    return buf_steal(&prompt);
}
