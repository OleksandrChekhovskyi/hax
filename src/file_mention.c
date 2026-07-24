/* SPDX-License-Identifier: MIT */
#include "file_mention.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "util.h"
#include "system/fs.h"
#include "system/path.h"
#include "system/spawn.h"
#include "terminal/input_core.h"

/* Candidate enumeration relative to a root directory (cwd by default).
 * `git ls-files` covers tracked plus untracked-but-not-ignored files
 * under the root; outside a repo it exits non-zero and the pruned
 * `find` takes over (its "./" prefix is stripped from the returned
 * selection in run_fzf).
 *
 * NUL-delimited end-to-end (-z / -print0, paired with fzf --read0
 * --print0): on line output git C-quotes any path with non-ASCII bytes
 * (core.quotePath), so e.g. café.txt would round-trip as the literal
 * text "caf\303\251.txt" — raw NUL-terminated records carry every
 * byte, newlines in filenames included.
 *
 * Deliberately a straight pipe into fzf: fzf reads stdin asynchronously
 * (keyboard/UI go via /dev/tty) and is interactive from the first line,
 * so even a slow walk of a huge tree never blocks the picker — do not
 * "optimize" this into collect-then-launch, which would introduce the
 * very stall it appears to avoid. */
#define CANDIDATES_CMD                                                                             \
    "git ls-files -z --cached --others --exclude-standard 2>/dev/null"                             \
    " || find . \\( -name .git -o -name node_modules \\) -prune -o -type f -print0 2>/dev/null"

/* Shared fzf invocation tacked onto the candidates producer. `%%` is a
 * literal % for --height; the final %s is the shell-quoted filter. */
#define FZF_TAIL " | fzf --read0 --print0 --height=~40%% --layout=reverse --scheme=path --query=%s"

/* Longest selection record accepted back from fzf. Paths beyond this
 * are pathological; a record that long is skipped rather than truncated
 * so a wrong (cut-off) path can never be picked. */
#define PICK_RECORD_MAX 4096

/* Probed fresh on every call — a stat-walk of $PATH costs microseconds,
 * and not caching means installing fzf mid-session just starts working
 * (and un-dims the /help row) without a restart. */
static int have_fzf(void)
{
    char *p = fs_which("fzf");
    if (!p)
        return 0;
    free(p);
    return 1;
}

/* Absolute / `~/…` / `../…` can't match cwd candidates — relocate.
 * A bare `/` mid-token is not enough (keeps typos like `mispted/x`
 * filterable against the project list). */
static int query_leaves_cwd(const char *q)
{
    if (!q || !*q)
        return 0;
    if (q[0] == '/')
        return 1;
    if (q[0] == '~' && (q[1] == '\0' || q[1] == '/'))
        return 1;
    if (q[0] == '.' && q[1] == '.' && (q[2] == '\0' || q[2] == '/'))
        return 1;
    return 0;
}

/* External query → root (through last '/') + filter suffix; bare `~` /
 * `..` become `~/` / `../`. Otherwise root is NULL and filter is query. */
static void split_query(const char *query, char **root_out, char **filter_out)
{
    if (!query)
        query = "";
    if (!query_leaves_cwd(query)) {
        *root_out = NULL;
        *filter_out = xstrdup(query);
        return;
    }
    const char *slash = strrchr(query, '/');
    if (!slash) {
        *root_out = xasprintf("%s/", query);
        *filter_out = xstrdup("");
        return;
    }
    size_t rlen = (size_t)(slash - query + 1); /* include the slash */
    char *root = xmalloc(rlen + 1);
    memcpy(root, query, rlen);
    root[rlen] = '\0';
    *root_out = root;
    *filter_out = xstrdup(slash + 1);
}

char *file_mention_fzf_cmd(const char *query)
{
    char *root = NULL, *filter = NULL;
    split_query(query, &root, &filter);

    char *q = shell_single_quote(filter);
    char *cmd;
    if (root) {
        /* expand_home before quoting — single quotes kill tilde expand */
        char *expanded = expand_home(root);
        char *r = shell_single_quote(expanded);
        /* Silent cd: bad prefix → empty picker, not "No such file". */
        cmd = xasprintf("{ cd %s 2>/dev/null && { " CANDIDATES_CMD "; }; }" FZF_TAIL, r, q);
        free(r);
        free(expanded);
    } else {
        /* --height keeps fzf inline; it draws on /dev/tty, stdout is the
         * selection. */
        cmd = xasprintf("{ " CANDIDATES_CMD "; }" FZF_TAIL, q);
    }
    free(q);
    free(root);
    free(filter);
    return cmd;
}

/* Read one NUL-terminated record from `f` into `buf`, returning 1, or
 * 0 on EOF with nothing read. A record exceeding the buffer is dropped
 * — returned as 1 with an empty buf, never truncated, so a cut-off
 * path can't be picked. A final unterminated record (EOF before NUL)
 * still counts, in case an fzf variant omits the trailing NUL. */
static int read_record(FILE *f, char *buf, size_t cap)
{
    size_t n = 0;
    int overflow = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF && n == 0 && !overflow)
            return 0;
        if (c == EOF || c == '\0') {
            buf[overflow ? 0 : n] = '\0';
            return 1;
        }
        if (n + 1 < cap)
            buf[n++] = (char)c;
        else
            overflow = 1;
    }
}

static char *run_fzf(const char *query)
{
    /* Same split as fzf_cmd; kept for rejoin, not threaded into the cmd
     * string, so the builder stays a pure sh -c payload. */
    char *root = NULL, *filter = NULL;
    split_query(query, &root, &filter);
    free(filter);

    char *cmd = file_mention_fzf_cmd(query);
    /* spawn_pipe_open_read (not popen): parent ignores SIGINT/SIGQUIT/
     * SIGPIPE for the duration, child resets them — Ctrl-C drives fzf,
     * and a still-streaming producer dies on SIGPIPE if fzf exits early. */
    struct spawn_pipe sp;
    char *out = NULL;
    if (spawn_pipe_open_read(&sp, cmd) == 0) {
        char rec[PICK_RECORD_MAX];
        if (read_record(sp.r, rec, sizeof(rec)) && *rec) {
            /* find(1) emits "./path"; strip to match git ls-files form. */
            const char *path = (rec[0] == '.' && rec[1] == '/') ? rec + 2 : rec;
            if (*path)
                out = root ? xasprintf("%s%s", root, path) : xstrdup(path);
        }
        int rc = spawn_pipe_close(&sp);
        /* fzf exits 0 only on a real selection (else 1/2/127/130). */
        if (!(WIFEXITED(rc) && WEXITSTATUS(rc) == 0)) {
            free(out);
            out = NULL;
        }
    }
    free(cmd);
    free(root);
    return out;
}

int file_mention_available(void)
{
    return have_fzf();
}

char *file_mention_pick(const char *query)
{
    if (!have_fzf()) {
        /* Deliberately no install command — package managers vary too
         * much for a one-liner to be right everywhere. */
        hax_warn("@file completion needs fzf installed");
        return NULL;
    }
    char *out = run_fzf(query);
    /* Validate the pick (stale index entry, deleted mid-pick), not each
     * candidate. expand_home first so a rejoined `~/…` still resolves. */
    if (out) {
        char *check = expand_home(out);
        if (ensure_regular_file(check) != 0) {
            hax_warn("cannot mention '%s': %s", out, strerror(errno));
            free(out);
            out = NULL;
        }
        free(check);
    }
    return out;
}

/* match phase: locate an `@`-starting token containing the cursor —
 * see the contract on file_mention_completer in the header. Pure, so
 * the editor can probe it before touching the screen. */
static int match_at_token(const char *buf, size_t len, size_t cursor, size_t *start, size_t *end,
                          void *user)
{
    (void)user;
    if (cursor == 0 || cursor > len)
        return 0;
    size_t s = cursor;
    while (s > 0 && !isspace((unsigned char)buf[s - 1]))
        s--;
    if (s >= cursor || buf[s] != '@')
        return 0;
    *start = s;
    *end = cursor;
    return 1;
}

/* pick phase: the token starts with '@' by construction of match. */
static char *pick_at_token(const char *token, void *user)
{
    (void)user;
    return file_mention_pick(token + 1);
}

const struct input_modal_completer file_mention_completer = {
    .match = match_at_token,
    .pick = pick_at_token,
};
