/* SPDX-License-Identifier: MIT */
#include "cmd_classify.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* Per-token classification. FORMAT applies only to pipeline-stripped
 * helpers; commands that pre-set the environment (`cd`, `env VAR=...`)
 * are handled separately by strip_neutral_prefix before classification. */
enum cmd_class {
    CC_READ,
    CC_LIST,
    CC_SEARCH,
    CC_FORMAT,
    CC_UNKNOWN,
};

/* Allowlists keyed by basename of argv[0]. Membership keeps the C
 * concise; exact matching is intentional — `gpg-grep` shouldn't sneak
 * into SEARCH because it shares a prefix. */
static int has(const char *const *list, size_t n, const char *t)
{
    for (size_t i = 0; i < n; i++)
        if (strcmp(list[i], t) == 0)
            return 1;
    return 0;
}

#define HAS(arr, t) has(arr, sizeof(arr) / sizeof(*(arr)), (t))

static int read_cmd(const char *t)
{
    static const char *const names[] = {"cat", "less", "more", "nl"};
    return HAS(names, t);
}

static int list_cmd(const char *t)
{
    /* `env` and `wc` deliberately omitted: `env` is consumed by
     * strip_neutral_prefix when used as a leading variable-setter, and
     * `wc` is in format_filter_cmd so it can pick up CC_READ when given
     * a file operand. */
    static const char *const names[] = {
        "ls",  "eza",      "exa",      "tree",     "find",    "fd",       "stat",    "file",
        "pwd", "realpath", "readlink", "which",    "whereis", "basename", "dirname", "du",
        "df",  "id",       "whoami",   "hostname", "uname",   "true",     "false",
    };
    return HAS(names, t);
}

static int search_cmd(const char *t)
{
    static const char *const names[] = {"grep", "egrep", "fgrep", "rg", "ag", "ack"};
    return HAS(names, t);
}

/* Format-only when used WITHOUT a file operand (i.e. as a pipeline
 * filter). With a file operand, head/tail/sed/awk become read-like
 * commands. The caller checks this only after stripping flags so a
 * trailing path can be detected as "non-flag, non-flag-value" token.
 *
 * Deliberately excluded: `tee` (always writes its operand), `xargs`
 * (runs an arbitrary wrapped command — handled specially below to
 * gate on whether the wrapped command is itself read-only). Including
 * them here would let `tee FILE` and `xargs rm` slip into the silent-
 * preview path. */
static int format_filter_cmd(const char *t)
{
    static const char *const names[] = {
        "wc",    "sort", "uniq", "cut", "tr",   "awk",    "sed",
        "head",  "tail", "tac",  "rev", "fold", "expand", "unexpand",
        "paste", "comm", "join", "yes", "echo", "printf", "column",
    };
    return HAS(names, t);
}

/* Classify the leading word of a segment after the cd/env strip. We
 * don't try to distinguish READ from LIST precisely (cat foo vs ls foo)
 * — both result in CC_READ/CC_LIST anyway and the caller treats them
 * uniformly. `git` gets a small subcommand allowlist — `git status` /
 * `git log` are useful but `git status` exit code carries info best
 * shown verbatim, so we keep them out of SEARCH/LIST and let them fall
 * through to the verbose preview. */
static enum cmd_class classify_word(const char *first, const char *second)
{
    if (read_cmd(first))
        return CC_READ;
    if (search_cmd(first))
        return CC_SEARCH;
    if (list_cmd(first))
        return CC_LIST;
    if (strcmp(first, "git") == 0 && second) {
        if (strcmp(second, "grep") == 0)
            return CC_SEARCH;
        if (strcmp(second, "ls-files") == 0)
            return CC_LIST;
    }
    return CC_UNKNOWN;
}

/* True for chars that end a top-level token. Used during quote-aware
 * scanning. */
static int is_word_end(char c)
{
    return c == ' ' || c == '\t' || c == '\0';
}

/* Scan past whitespace in [s, end). */
static const char *skip_ws(const char *s, const char *end)
{
    while (s < end && (*s == ' ' || *s == '\t'))
        s++;
    return s;
}

/* Extract the first whitespace-delimited word in [s, end), respecting
 * single quotes, double quotes, and backslash escapes (so a quoted path
 * containing spaces stays one token). Returns malloc'd word with quotes
 * stripped, or NULL on empty input. *out_next set to the position
 * after the word. */
static char *take_word(const char *s, const char *end, const char **out_next)
{
    s = skip_ws(s, end);
    if (s >= end) {
        *out_next = s;
        return NULL;
    }
    struct buf b;
    buf_init(&b);
    char quote = 0;
    while (s < end) {
        char c = *s;
        if (quote) {
            if (c == quote) {
                quote = 0;
                s++;
                continue;
            }
            if (quote == '"' && c == '\\' && s + 1 < end) {
                /* Backslash inside double quotes only escapes a few
                 * specific chars per POSIX; for our display heuristic
                 * we just take the next byte verbatim. */
                buf_append(&b, s + 1, 1);
                s += 2;
                continue;
            }
            buf_append(&b, &c, 1);
            s++;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            s++;
            continue;
        }
        if (c == '\\' && s + 1 < end) {
            buf_append(&b, s + 1, 1);
            s += 2;
            continue;
        }
        if (is_word_end(c))
            break;
        buf_append(&b, &c, 1);
        s++;
    }
    *out_next = s;
    if (b.len == 0) {
        buf_free(&b);
        return NULL;
    }
    char nul = 0;
    buf_append(&b, &nul, 1);
    return buf_steal(&b);
}

/* Return 1 if the rest of the segment (after stripping flags) contains
 * any non-flag token — i.e. a file operand. Used to decide whether a
 * head/tail/sed/awk segment is "format-only" or "read-from-file". */
static int has_non_flag_operand(const char *s, const char *end)
{
    const char *p = s;
    while (p < end) {
        char *w = take_word(p, end, &p);
        if (!w)
            break;
        int is_flag = w[0] == '-';
        free(w);
        if (!is_flag)
            return 1;
    }
    return 0;
}

/* Reject characters/sequences that signal we're not just running a
 * tidy pipeline of read-only utilities. The check is intentionally
 * lexical (no real shell parsing) — false positives just push the
 * command into the verbose-preview path, which is the safe direction. */
static int has_disqualifier(const char *s)
{
    char quote = 0;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && s[i + 1]) {
                i++; /* skip escaped */
                continue;
            }
            if (c == quote) {
                quote = 0;
                continue;
            }
            /* Single quotes suppress all expansion, but double quotes
             * still expand `$(...)` and backticks — the shell runs them
             * exactly as it would unquoted. Process substitution
             * `<(...)`/`>(...)` is bash-specific and only recognized
             * outside quotes, so we don't flag those here. */
            if (quote == '"') {
                if (c == '`')
                    return 1;
                if (c == '$' && s[i + 1] == '(')
                    return 1;
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (c == '\\' && s[i + 1]) {
            i++;
            continue;
        }
        /* Command/process substitution and heredocs. */
        if (c == '`')
            return 1;
        if (c == '$' && s[i + 1] == '(')
            return 1;
        if ((c == '<' || c == '>') && s[i + 1] == '(')
            return 1;
        if (c == '<' && s[i + 1] == '<')
            return 1;
        /* Standalone `&` backgrounds the preceding command and runs
         * the remainder. Tolerate it only as part of `&&` (either
         * half) or as the suffix of `2>&` stderr-merge. */
        if (c == '&' && s[i + 1] != '&' && (i == 0 || (s[i - 1] != '>' && s[i - 1] != '&')))
            return 1;
        /* Output redirection. Tolerate the stderr-merge forms `2>&1`
         * and `2>FILE` only when FILE is /dev/null (very common in
         * grep/find pipelines to suppress permission-denied noise).
         * Boundary check on the `2`: it must be its own token (preceded
         * by whitespace or one of |&; or be at the very start), so
         * `python2 >foo` doesn't masquerade as `python 2>foo`. */
        if (c == '>') {
            int has_2_prefix =
                i >= 1 && s[i - 1] == '2' && (i == 1 || strchr(" \t&|;", s[i - 2]) != NULL);
            if (has_2_prefix) {
                /* `2>&1` */
                if (s[i + 1] == '&' && s[i + 2] == '1')
                    continue;
                /* `2>/dev/null` (allow optional whitespace) */
                const char *p = s + i + 1;
                while (*p == ' ' || *p == '\t')
                    p++;
                if (strncmp(p, "/dev/null", 9) == 0) {
                    i = (size_t)(p + 9 - s) - 1;
                    continue;
                }
            }
            return 1;
        }
    }
    return 0;
}

/* Strip leading `cd <path>`, `pushd <path>`, `popd`, and `env VAR=val`
 * (one or more) from the segment. Returns the start of the remaining
 * command, or NULL if the strip consumed everything (segment was just
 * `cd foo` — neutral, no command, treated as successful no-op).
 *
 * Sets *only_neutral = 1 when the entire segment was neutral. */
static const char *strip_neutral_prefix(const char *s, const char *end, int *only_neutral)
{
    *only_neutral = 0;
    int consumed_any = 0;
    for (;;) {
        const char *probe = s;
        char *w = take_word(probe, end, &probe);
        if (!w) {
            /* Empty input: only_neutral is true iff we actually
             * consumed a neutral prefix (e.g. `cd /tmp` standalone).
             * A purely empty/whitespace segment stays unclassified. */
            *only_neutral = consumed_any;
            return NULL;
        }
        int is_neutral = 0;
        if (strcmp(w, "cd") == 0 || strcmp(w, "pushd") == 0) {
            /* Take exactly one path argument (skip flag-like tokens
             * such as `pushd -n dir`). */
            const char *p = probe;
            for (;;) {
                char *a = take_word(p, end, &p);
                if (!a)
                    break;
                int is_flag = a[0] == '-';
                free(a);
                if (!is_flag) {
                    is_neutral = 1;
                    break;
                }
            }
            if (!is_neutral) {
                /* `cd` with no operand is `cd $HOME` — also neutral. */
                is_neutral = 1;
                p = probe;
                while (p < end && (*p == ' ' || *p == '\t'))
                    p++;
            }
            probe = p;
        } else if (strcmp(w, "popd") == 0) {
            is_neutral = 1;
        } else if (strchr(w, '=') != NULL && (w[0] == '_' || isalpha((unsigned char)w[0]))) {
            /* VAR=value as a leading bare assignment. */
            is_neutral = 1;
        } else if (strcmp(w, "env") == 0) {
            /* `env VAR=val ... cmd args` — consume VAR=val tokens, then
             * leave `cmd args` for the next iteration. */
            const char *p = probe;
            for (;;) {
                const char *save = p;
                char *a = take_word(p, end, &p);
                if (!a)
                    break;
                int is_assign =
                    strchr(a, '=') != NULL && (a[0] == '_' || isalpha((unsigned char)a[0]));
                free(a);
                if (!is_assign) {
                    p = save;
                    break;
                }
            }
            is_neutral = 1;
            probe = p;
        }
        free(w);
        if (!is_neutral)
            return s;
        consumed_any = 1;
        s = probe;
        s = skip_ws(s, end);
        if (s >= end) {
            *only_neutral = 1;
            return NULL;
        }
    }
}

/* `xargs CMD ARGS` runs CMD, which is only safe when CMD is itself
 * read-only. Skip xargs's flags and look up the first non-flag token
 * against our allowlists. Doesn't fully model xargs flag grammar
 * (flags-with-values like `-n N` would consume N as a "wrapped
 * command" candidate); the lookup naturally rejects those — `1` isn't
 * a known command — and the misclassification direction is safe
 * (UNKNOWN falls back to verbose preview). */
static enum cmd_class classify_xargs(const char *body, const char *seg_end)
{
    const char *p = body;
    char *first = take_word(p, seg_end, &p);
    free(first); /* "xargs" */
    while (p < seg_end) {
        char *w = take_word(p, seg_end, &p);
        if (!w)
            return CC_UNKNOWN;
        int is_flag = w[0] == '-' && w[1] != '\0';
        if (!is_flag) {
            enum cmd_class c = read_cmd(w)     ? CC_READ
                               : search_cmd(w) ? CC_SEARCH
                               : list_cmd(w)   ? CC_LIST
                                               : CC_UNKNOWN;
            free(w);
            return c;
        }
        free(w);
    }
    return CC_UNKNOWN;
}

/* find/fd actions that run arbitrary commands or mutate the
 * filesystem. We don't try to peek into -exec's wrapped command —
 * even `-exec grep` would need argument-boundary parsing for the
 * trailing `;` / `+`. Conservative: any of these forces verbose.
 *
 * `-fprint`/`-fprint0`/`-fprintf`/`-fls` create or truncate a named
 * output file (vs `-print*` which write to stdout); they're rare but
 * legitimately mutate the working tree. */
static int find_is_mutating(const char *body, const char *seg_end)
{
    const char *p = body;
    char *first = take_word(p, seg_end, &p);
    free(first); /* "find" / "fd" */
    while (p < seg_end) {
        char *w = take_word(p, seg_end, &p);
        if (!w)
            return 0;
        /* `--exec=CMD` and `--exec-batch=CMD` are valid clap-style
         * spellings that fd accepts equivalently to the space-separated
         * form, so prefix-match those alongside the bare flag. */
        int hit = strcmp(w, "-delete") == 0 || strcmp(w, "-exec") == 0 ||
                  strcmp(w, "-execdir") == 0 || strcmp(w, "-ok") == 0 || strcmp(w, "-okdir") == 0 ||
                  strcmp(w, "-fprint") == 0 || strcmp(w, "-fprint0") == 0 ||
                  strcmp(w, "-fprintf") == 0 || strcmp(w, "-fls") == 0 || strcmp(w, "-x") == 0 ||
                  strcmp(w, "-X") == 0 || strcmp(w, "--exec") == 0 ||
                  strcmp(w, "--exec-batch") == 0 || strncmp(w, "--exec=", 7) == 0 ||
                  strncmp(w, "--exec-batch=", 13) == 0;
        free(w);
        if (hit)
            return 1;
    }
    return 0;
}

/* Detect `-o FILE`, `-oFILE`, `--output FILE`, `--output=FILE`. Used
 * by commands where this is the standard write-to-file flag and no
 * other short option starts with `o` (sort, tree). Caller is
 * responsible for skipping the leader word. */
static int has_dash_o_output_flag(const char *p, const char *seg_end)
{
    while (p < seg_end) {
        char *w = take_word(p, seg_end, &p);
        if (!w)
            return 0;
        int hit = (w[0] == '-' && w[1] == 'o' && (w[2] == '\0' || w[2] != '-')) ||
                  strcmp(w, "--output") == 0 || strncmp(w, "--output=", 9) == 0;
        free(w);
        if (hit)
            return 1;
    }
    return 0;
}

/* Format-filter commands that, despite a file operand, are writing
 * via a flag (sort -o FILE, awk -i inplace ...). Called from the
 * format-filter branch of classify_segment before we'd return CC_READ.
 *
 * This covers flag-based writes; in-program redirections that hide
 * inside a quoted script body — `awk '{print > "f"}'`, `sed -e 'w f'`
 * — aren't visible to the lexer and stay accepted. The classifier is
 * conservative-not-perfect by design (see top of file). */
static int format_filter_is_writing(const char *body, const char *seg_end, const char *cmd)
{
    const char *p = body;
    char *first = take_word(p, seg_end, &p);
    free(first); /* leader */

    if (strcmp(cmd, "sort") == 0) {
        return has_dash_o_output_flag(p, seg_end);
    } else if (strcmp(cmd, "awk") == 0) {
        /* GNU awk: `-i inplace` enables the inplace extension and
         * mutates the file operand. Plain `-i FILE` (load library)
         * is read-only, so we only flag the literal value "inplace".
         * Both spellings work: separated `-i inplace` and joined
         * `-iinplace` (getopt-style required-argument cluster). */
        while (p < seg_end) {
            char *w = take_word(p, seg_end, &p);
            if (!w)
                return 0;
            if (strcmp(w, "-i") == 0) {
                free(w);
                char *next = take_word(p, seg_end, &p);
                if (!next)
                    return 0;
                int hit = strcmp(next, "inplace") == 0;
                free(next);
                if (hit)
                    return 1;
                continue;
            }
            int joined = w[0] == '-' && w[1] == 'i' && strcmp(w + 2, "inplace") == 0;
            free(w);
            if (joined)
                return 1;
        }
    }
    return 0;
}

/* tree's `-o FILE` writes the listing to a file. Same shape as
 * sort's `-o`. Called for tree before classify_segment returns
 * CC_LIST. */
static int tree_is_writing(const char *body, const char *seg_end)
{
    const char *p = body;
    char *first = take_word(p, seg_end, &p);
    free(first); /* "tree" */
    return has_dash_o_output_flag(p, seg_end);
}

/* less copies its input to a named file with `-o FILE` (or `-O FILE`
 * for force-overwrite, or `--log-file=FILE`). Also called for `more`
 * since some distros symlink it to less, in which case the same flags
 * apply; on systems where `more` is the standalone util-linux/BSD
 * tool, none of these flags exist and the command would just error —
 * harmless false-positive disqualification. */
static int less_is_writing(const char *body, const char *seg_end)
{
    const char *p = body;
    char *first = take_word(p, seg_end, &p);
    free(first); /* "less" / "more" */
    while (p < seg_end) {
        char *w = take_word(p, seg_end, &p);
        if (!w)
            return 0;
        int hit = (w[0] == '-' && (w[1] == 'o' || w[1] == 'O') && (w[2] == '\0' || w[2] != '-')) ||
                  strcmp(w, "--log-file") == 0 || strncmp(w, "--log-file=", 11) == 0;
        free(w);
        if (hit)
            return 1;
    }
    return 0;
}

/* sed with -i (in-place edit) mutates the file operand. Detect any
 * GNU/BSD spelling: `-i`, `-iSUFFIX` (BSD), combined short flags
 * (`-Ei`, `-Eni`), `--in-place`, `--in-place=SUFFIX`.
 *
 * For short-option clusters we scan chars after the dash up to a
 * `.` (which begins a BSD suffix on `-i.bak` / `-Ei.bak`); any `i`
 * before the `.` means in-place. */
static int sed_is_in_place(const char *body, const char *seg_end)
{
    const char *p = body;
    char *first = take_word(p, seg_end, &p);
    free(first); /* "sed" */
    while (p < seg_end) {
        char *w = take_word(p, seg_end, &p);
        if (!w)
            return 0;
        int hit = 0;
        if (strncmp(w, "--in-place", 10) == 0) {
            hit = 1;
        } else if (w[0] == '-' && w[1] != '-' && w[1] != '\0') {
            for (size_t i = 1; w[i] && w[i] != '.'; i++) {
                if (w[i] == 'i') {
                    hit = 1;
                    break;
                }
            }
        }
        free(w);
        if (hit)
            return 1;
    }
    return 0;
}

/* Classify one [seg, seg_end) segment. The result is:
 *   CC_FORMAT  if the segment is a pipeline filter (head -n 5, wc -l, …)
 *   CC_READ/CC_LIST/CC_SEARCH  for known exploration leaders
 *   CC_UNKNOWN otherwise — caller treats as disqualifier. */
static enum cmd_class classify_segment(const char *seg, const char *seg_end)
{
    int only_neutral = 0;
    const char *body = strip_neutral_prefix(seg, seg_end, &only_neutral);
    if (only_neutral)
        return CC_LIST; /* `cd foo` standalone — no-op exploration step */
    if (!body)
        return CC_UNKNOWN; /* empty segment from malformed input */

    const char *p = body;
    char *w1 = take_word(p, seg_end, &p);
    if (!w1)
        return CC_UNKNOWN;
    char *w2 = take_word(p, seg_end, &p);

    enum cmd_class c = classify_word(w1, w2);
    if (c != CC_UNKNOWN) {
        if ((strcmp(w1, "find") == 0 || strcmp(w1, "fd") == 0) && find_is_mutating(body, seg_end)) {
            free(w1);
            free(w2);
            return CC_UNKNOWN;
        }
        if (strcmp(w1, "tree") == 0 && tree_is_writing(body, seg_end)) {
            free(w1);
            free(w2);
            return CC_UNKNOWN;
        }
        if ((strcmp(w1, "less") == 0 || strcmp(w1, "more") == 0) &&
            less_is_writing(body, seg_end)) {
            free(w1);
            free(w2);
            return CC_UNKNOWN;
        }
        free(w1);
        free(w2);
        return c;
    }

    /* xargs: classify based on the wrapped command. */
    if (strcmp(w1, "xargs") == 0) {
        free(w1);
        free(w2);
        return classify_xargs(body, seg_end);
    }

    /* sed -i mutates its file operand — force UNKNOWN even with one. */
    if (strcmp(w1, "sed") == 0 && sed_is_in_place(body, seg_end)) {
        free(w1);
        free(w2);
        return CC_UNKNOWN;
    }

    /* Format-filter check: head/tail/sed/awk become reads when given a
     * file operand, so reclassify those as CC_READ; otherwise CC_FORMAT.
     * A few format-filter commands have flags that turn them into
     * writers (`sort -o`, `awk -i inplace`) — those force UNKNOWN even
     * with a file operand. */
    if (format_filter_cmd(w1)) {
        const char *rest = body;
        char *consume = take_word(rest, seg_end, &rest);
        free(consume);
        if (has_non_flag_operand(rest, seg_end)) {
            if (format_filter_is_writing(body, seg_end, w1)) {
                free(w1);
                free(w2);
                return CC_UNKNOWN;
            }
            free(w1);
            free(w2);
            return CC_READ;
        }
        free(w1);
        free(w2);
        return CC_FORMAT;
    }

    free(w1);
    free(w2);
    return CC_UNKNOWN;
}

/* Walk `s`, splitting on top-level connectors (&&, ||, ;, |) while
 * respecting quotes. Calls `cb(seg, end, user)` for each segment;
 * stops early and returns 0 if cb returns 0. Returns 1 on full walk. */
static int for_each_segment(const char *s, int (*cb)(const char *, const char *, void *),
                            void *user)
{
    size_t n = strlen(s);
    const char *end = s + n;
    const char *seg = s;
    char quote = 0;
    for (size_t i = 0; i < n;) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && s[i + 1])
                i += 2;
            else if (c == quote) {
                quote = 0;
                i++;
            } else {
                i++;
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            i++;
            continue;
        }
        if (c == '\\' && s[i + 1]) {
            i += 2;
            continue;
        }
        int split_len = 0;
        if ((c == '&' && s[i + 1] == '&') || (c == '|' && s[i + 1] == '|'))
            split_len = 2;
        else if (c == ';' || c == '|')
            split_len = 1;
        if (split_len) {
            if (!cb(seg, s + i, user))
                return 0;
            seg = s + i + split_len;
            i += (size_t)split_len;
            continue;
        }
        i++;
    }
    return cb(seg, end, user);
}

struct walk_state {
    int verdict; /* 1 = still exploration, 0 = give up */
};

static int walk_cb(const char *seg, const char *end, void *user)
{
    struct walk_state *ws = user;
    enum cmd_class c = classify_segment(seg, end);
    if (c == CC_UNKNOWN) {
        ws->verdict = 0;
        return 0;
    }
    /* CC_FORMAT segments are pipeline filters and contribute nothing —
     * keep walking. CC_READ/CC_LIST/CC_SEARCH all qualify. */
    return 1;
}

int cmd_is_exploration(const char *cmd)
{
    if (!cmd || !*cmd)
        return 0;
    if (has_disqualifier(cmd))
        return 0;
    struct walk_state ws = {.verdict = 1};
    for_each_segment(cmd, walk_cb, &ws);
    return ws.verdict;
}
