/* SPDX-License-Identifier: MIT */
#include "env.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "utf8_sanitize.h"
#include "util.h"

/* Per-file cap for AGENTS.md content. A single file larger than this is
 * almost certainly a mistake; truncating with a marker is more useful than
 * blowing up the prompt. */
#define AGENTS_MD_FILE_CAP (64u * 1024u)

/* Maximum directory levels walked upward from cwd. Bounds the cost of a
 * runaway walk on a deep tree without any project marker. */
#define AGENTS_MD_MAX_LEVELS 64

/* Only the head of SKILL.md is read — we just need YAML frontmatter, never
 * the full skill body. Keep this comfortably above realistic frontmatter
 * sizes. */
#define SKILL_FRONTMATTER_HEAD 8192

/* Spec limit for the description field; longer values are truncated. */
#define SKILL_DESCRIPTION_MAX 1024

static int env_flag_set(const char *name)
{
    const char *v = getenv(name);
    return v && *v;
}

/* Walk cwd → filesystem root, returning a malloc'd path to the first
 * directory containing `.git`. Returns NULL if no marker is found within
 * AGENTS_MD_MAX_LEVELS. Shared by the env block (is_git_repo) and the
 * AGENTS.md walk so both agree on what counts as "in a repo". */
static char *find_project_root(const char *cwd)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", cwd);
    for (int i = 0; i < AGENTS_MD_MAX_LEVELS; i++) {
        char marker[PATH_MAX + 16];
        snprintf(marker, sizeof(marker), "%s/.git", dir);
        struct stat st;
        if (stat(marker, &st) == 0)
            return xstrdup(dir);

        char *slash = strrchr(dir, '/');
        if (!slash)
            break;
        if (slash == dir) {
            /* "/foo" → "/"; "/" → done. */
            if (dir[1] == '\0')
                break;
            dir[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
    return NULL;
}

static void append_env_block(struct buf *b, const char *model)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        snprintf(cwd, sizeof(cwd), "(unknown)");

    struct utsname u;
    const char *os_name = "unknown", *os_release = "";
    if (uname(&u) == 0) {
        os_name = u.sysname;
        os_release = u.release;
    }

    const char *shell = getenv("SHELL");
    if (!shell || !*shell)
        shell = "/bin/sh";

    char date[16];
    time_t t = time(NULL);
    struct tm tm_;
    if (localtime_r(&t, &tm_))
        strftime(date, sizeof(date), "%Y-%m-%d", &tm_);
    else
        snprintf(date, sizeof(date), "unknown");

    char *project_root = find_project_root(cwd);
    int git = project_root != NULL;
    free(project_root);

    /* Linux paths are arbitrary byte sequences and env vars are user-set,
     * so any of cwd / shell / model could carry non-UTF-8 bytes. Jansson
     * rejects non-UTF-8 and embedded NULs, so clean each before splicing.
     * uname and strftime outputs are POSIX-defined to be ASCII and don't
     * need this treatment. */
    char *cwd_clean = sanitize_utf8(cwd, strlen(cwd));
    char *shell_clean = sanitize_utf8(shell, strlen(shell));
    char *model_clean = (model && *model) ? sanitize_utf8(model, strlen(model)) : NULL;

    buf_append_str(b, "<env>\n");
    char *line = xasprintf("  cwd: %s\n", cwd_clean);
    buf_append_str(b, line);
    free(line);
    line = xasprintf("  os: %s %s\n", os_name, os_release);
    buf_append_str(b, line);
    free(line);
    line = xasprintf("  shell: %s\n", shell_clean);
    buf_append_str(b, line);
    free(line);
    if (model_clean) {
        line = xasprintf("  model: %s\n", model_clean);
        buf_append_str(b, line);
        free(line);
    }
    line = xasprintf("  date: %s\n", date);
    buf_append_str(b, line);
    free(line);
    line = xasprintf("  is_git_repo: %s\n", git ? "yes" : "no");
    buf_append_str(b, line);
    free(line);
    buf_append_str(b, "</env>\n");

    free(cwd_clean);
    free(shell_clean);
    free(model_clean);
}

/* Append a single AGENTS.md file under a `## <path>` header. Returns 1 if
 * the file existed and was appended, 0 otherwise. The first successful
 * call also writes the `# Project Context` section header (and a leading
 * separator if the buffer already has env-block content). */
static int append_agents_md(struct buf *b, const char *path, int *seen_header)
{
    size_t n = 0;
    int truncated = 0;
    char *content = slurp_file_capped(path, AGENTS_MD_FILE_CAP, &n, &truncated);
    if (!content)
        return 0;

    /* AGENTS.md is user-authored and may contain embedded NULs or invalid
     * UTF-8; the path itself comes from getcwd / $HOME / $XDG_CONFIG_HOME
     * which on Linux can also carry arbitrary bytes. Both would break
     * provider JSON (NUL truncates strlen, Jansson rejects non-UTF-8) —
     * sanitize both before splicing into the prompt. */
    char *clean = sanitize_utf8(content, n);
    free(content);
    size_t clean_len = strlen(clean);
    char *path_clean = sanitize_utf8(path, strlen(path));

    if (!*seen_header) {
        if (b->len > 0)
            buf_append_str(b, "\n");
        buf_append_str(b, "# Project Context\n");
        *seen_header = 1;
    }
    buf_append_str(b, "\n## ");
    buf_append_str(b, path_clean);
    buf_append_str(b, "\n\n");
    buf_append(b, clean, clean_len);
    /* Ensure trailing newline before the next section header. */
    if (clean_len == 0 || clean[clean_len - 1] != '\n')
        buf_append_str(b, "\n");
    if (truncated)
        buf_append_str(b, "[truncated]\n");
    free(clean);
    free(path_clean);
    return 1;
}

static char *global_agents_md_path(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return xasprintf("%s/hax/AGENTS.md", xdg);
    const char *home = getenv("HOME");
    if (home && *home)
        return xasprintf("%s/.config/hax/AGENTS.md", home);
    return NULL;
}

static void append_project_agents_md(struct buf *b, int *seen_header)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        return;

    char *root = find_project_root(cwd);
    if (!root) {
        /* No project marker — only the cwd-level file (if any) is
         * considered, to avoid pulling in unrelated AGENTS.md files when
         * hax is run outside any repo. */
        char *candidate = xasprintf("%s/AGENTS.md", cwd);
        append_agents_md(b, candidate, seen_header);
        free(candidate);
        return;
    }

    /* Collect every AGENTS.md from cwd up to and including the project
     * root, then emit farthest-first so closer files take precedence.
     * append_agents_md handles missing/non-regular paths via slurp_*'s
     * own guard, so we don't pre-filter here. */
    char *paths[AGENTS_MD_MAX_LEVELS];
    int n = 0;
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", cwd);
    for (int i = 0; i < AGENTS_MD_MAX_LEVELS; i++) {
        paths[n++] = xasprintf("%s/AGENTS.md", dir);

        if (strcmp(dir, root) == 0)
            break;

        char *slash = strrchr(dir, '/');
        if (!slash)
            break;
        if (slash == dir) {
            if (dir[1] == '\0')
                break;
            dir[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
    free(root);

    for (int i = n - 1; i >= 0; i--) {
        append_agents_md(b, paths[i], seen_header);
        free(paths[i]);
    }
}

/* Extract the `description:` value from YAML frontmatter at the head of
 * `content`. Frontmatter is delimited by `---` on its own line at the
 * start, terminated by a matching `---`. Only single-line scalar values
 * are supported (no `description: |` blocks); single- or double-quoted
 * values are unquoted; whitespace is trimmed. Returns a freshly-allocated
 * string clamped to SKILL_DESCRIPTION_MAX, or NULL if the field is absent
 * or the file lacks frontmatter. */
static char *parse_skill_description(const char *content, size_t len)
{
    /* Accept LF or CRLF after the opening fence — the closer below already
     * tolerates an optional \r, so be symmetric. */
    const char *p;
    if (len >= 4 && memcmp(content, "---\n", 4) == 0)
        p = content + 4;
    else if (len >= 5 && memcmp(content, "---\r\n", 5) == 0)
        p = content + 5;
    else
        return NULL;

    const char *end = content + len;
    while (p < end) {
        const char *line_end = memchr(p, '\n', end - p);
        if (!line_end)
            line_end = end;
        size_t line_len = line_end - p;

        /* End-of-frontmatter marker. Accept `---` with optional CR. */
        if ((line_len == 3 && memcmp(p, "---", 3) == 0) ||
            (line_len == 4 && memcmp(p, "---\r", 4) == 0))
            return NULL;

        if (line_len > 12 && memcmp(p, "description:", 12) == 0) {
            const char *v = p + 12;
            const char *vend = line_end;
            while (v < vend && (*v == ' ' || *v == '\t'))
                v++;
            while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t' || vend[-1] == '\r'))
                vend--;
            if (vend - v >= 2 &&
                ((*v == '"' && vend[-1] == '"') || (*v == '\'' && vend[-1] == '\''))) {
                v++;
                vend--;
            }
            if (vend <= v)
                return NULL;
            size_t n = (size_t)(vend - v);
            if (n > SKILL_DESCRIPTION_MAX)
                n = SKILL_DESCRIPTION_MAX;
            /* sanitize_utf8 strips NULs and replaces invalid sequences with
             * U+FFFD, so the description is safe to splice into the prompt
             * JSON regardless of what the file contains. */
            return sanitize_utf8(v, n);
        }
        p = line_end + 1;
    }
    return NULL;
}

struct skill_entry {
    char *dir;          /* directory name, used as identifier */
    char *display_path; /* path to SKILL.md as shown in the prompt */
    char *description;  /* may be NULL */
};

static int cmp_skill_entry(const void *a, const void *b)
{
    const struct skill_entry *sa = a;
    const struct skill_entry *sb = b;
    return strcmp(sa->dir, sb->dir);
}

/* Scan one root for skills. For each `<root>/<name>/SKILL.md` regular
 * file, append a fresh entry to *out (xrealloc'd as needed). Skips
 * dotfiles and entries already present in *out (so an earlier root takes
 * precedence over a later one — used to let project skills shadow
 * same-named global ones). `root` is also what's shown to the model in
 * the entry's path, so callers pass a relative path for project skills
 * and an absolute path for global ones. */
static void collect_skills(struct skill_entry **out, size_t *n, size_t *cap, const char *root)
{
    DIR *d = opendir(root);
    if (!d)
        return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        /* readdir bytes (and `root` from $HOME/$XDG) can be non-UTF-8 on
         * Linux. Sanitize the dir name up front so dedup, sort, and prompt
         * emission all see the same clean identifier; sanitize the path
         * after we've read the file so opendir/stat still see raw bytes. */
        char *dir_clean = sanitize_utf8(ent->d_name, strlen(ent->d_name));

        int already = 0;
        for (size_t i = 0; i < *n; i++) {
            if (strcmp((*out)[i].dir, dir_clean) == 0) {
                already = 1;
                break;
            }
        }
        if (already) {
            free(dir_clean);
            continue;
        }

        char *skill_md = xasprintf("%s/%s/SKILL.md", root, ent->d_name);
        size_t md_len = 0;
        int truncated = 0;
        char *md = slurp_file_capped(skill_md, SKILL_FRONTMATTER_HEAD, &md_len, &truncated);
        if (!md) {
            /* Missing, non-regular, or unreadable — slurp_* sets errno;
             * we just skip the entry. */
            free(dir_clean);
            free(skill_md);
            continue;
        }
        char *desc = parse_skill_description(md, md_len);
        free(md);

        char *path_clean = sanitize_utf8(skill_md, strlen(skill_md));
        free(skill_md);

        if (*n == *cap) {
            size_t c = *cap ? *cap * 2 : 8;
            *out = xrealloc(*out, c * sizeof(**out));
            *cap = c;
        }
        (*out)[*n].dir = dir_clean;
        (*out)[*n].display_path = path_clean;
        (*out)[*n].description = desc;
        (*n)++;
    }
    closedir(d);
}

static char *global_skills_root(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return xasprintf("%s/hax/skills", xdg);
    const char *home = getenv("HOME");
    if (home && *home)
        return xasprintf("%s/.config/hax/skills", home);
    return NULL;
}

static void append_skills(struct buf *b)
{
    struct skill_entry *skills = NULL;
    size_t n = 0, cap = 0;

    /* Project first so its entries shadow same-named global ones. */
    collect_skills(&skills, &n, &cap, ".agents/skills");

    char *global = global_skills_root();
    if (global) {
        collect_skills(&skills, &n, &cap, global);
        free(global);
    }

    if (n == 0)
        return;

    qsort(skills, n, sizeof(*skills), cmp_skill_entry);

    if (b->len > 0)
        buf_append_str(b, "\n");
    buf_append_str(b, "# Skills\n\n"
                      "Read the corresponding SKILL.md when a task matches the description:\n\n");
    for (size_t i = 0; i < n; i++) {
        char *line;
        if (skills[i].description)
            line = xasprintf("- %s: %s (%s)\n", skills[i].dir, skills[i].description,
                             skills[i].display_path);
        else
            line = xasprintf("- %s (%s)\n", skills[i].dir, skills[i].display_path);
        buf_append_str(b, line);
        free(line);
        free(skills[i].dir);
        free(skills[i].display_path);
        free(skills[i].description);
    }
    free(skills);
}

char *env_build_suffix(const char *model)
{
    int do_env = !env_flag_set("HAX_NO_ENV");
    int do_agents = !env_flag_set("HAX_NO_AGENTS_MD");

    if (!do_env && !do_agents)
        return NULL;

    struct buf b;
    buf_init(&b);

    if (do_env)
        append_env_block(&b, model);

    if (do_agents) {
        int seen_header = 0;
        char *global = global_agents_md_path();
        if (global) {
            append_agents_md(&b, global, &seen_header);
            free(global);
        }
        append_project_agents_md(&b, &seen_header);
        append_skills(&b);
    }

    if (b.len == 0) {
        buf_free(&b);
        return NULL;
    }
    return buf_steal(&b);
}
