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
#include "system/spawn.h"
#include "terminal/input_core.h"

/* Candidate enumeration, cwd-relative. `git ls-files` covers tracked
 * plus untracked-but-not-ignored files under the cwd; outside a repo it
 * exits non-zero and the pruned `find` takes over (its "./" prefix is
 * stripped from the returned selection in run_fzf).
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

char *file_mention_fzf_cmd(const char *query)
{
    char *q = shell_single_quote(query);
    /* --height keeps fzf inline below the prompt instead of taking the
     * alternate screen; ~40% shrinks to the candidate count. fzf draws
     * on /dev/tty, so its stdout stays clean for the selection. */
    char *cmd = xasprintf("{ " CANDIDATES_CMD "; } | fzf --read0 --print0"
                          " --height=~40%% --layout=reverse --scheme=path --query=%s",
                          q);
    free(q);
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
    char *cmd = file_mention_fzf_cmd(query);
    /* spawn_pipe_open_read (not popen) so the pipeline gets the full
     * spawn signal etiquette: parent ignores SIGINT/SIGQUIT/SIGPIPE for
     * the duration, child resets them to default — so terminal Ctrl-C
     * drives fzf, not hax, and a still-streaming producer dies cleanly
     * on SIGPIPE when fzf exits early. */
    struct spawn_pipe sp;
    char *out = NULL;
    if (spawn_pipe_open_read(&sp, cmd) == 0) {
        char rec[PICK_RECORD_MAX];
        if (read_record(sp.r, rec, sizeof(rec)) && *rec) {
            /* The find(1) fallback emits "./path"; normalize to the
             * bare relative form git ls-files produces. */
            const char *path = (rec[0] == '.' && rec[1] == '/') ? rec + 2 : rec;
            if (*path)
                out = xstrdup(path);
        }
        int rc = spawn_pipe_close(&sp);
        /* fzf exits 0 only on a real selection (1 = no match, 2 =
         * error, 127 = not found after all, 130 = cancelled). */
        if (!(WIFEXITED(rc) && WEXITSTATUS(rc) == 0)) {
            free(out);
            out = NULL;
        }
    }
    free(cmd);
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
    /* The index can offer paths gone from the working tree (a tracked
     * file deleted but not staged), and any file can vanish while the
     * picker is open — so validate the selection, not the candidate
     * stream: one stat instead of one per candidate, and it covers the
     * mid-pick race too. */
    if (out && ensure_regular_file(out) != 0) {
        hax_warn("cannot mention '%s': %s", out, strerror(errno));
        free(out);
        out = NULL;
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
