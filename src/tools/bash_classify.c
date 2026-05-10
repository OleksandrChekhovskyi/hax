/* SPDX-License-Identifier: MIT */
#include "tools/bash_classify.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* Per-segment classification used by bash_cmd_is_exploration. The whole
 * module is an approximation tuned for common cases — it is NOT a
 * security boundary. Bash runs whatever the model sends either way;
 * this just keeps obvious read-only probes (`ls`, `cat foo.c`,
 * `grep -r foo src`) from clogging scrollback with verbose previews.
 *
 * Pipeline shape:
 *   1. has_disqualifier rejects whole-string danger signals: command
 *      substitution `$(...)` and backticks (also detected inside
 *      double quotes — they expand there too), process substitution
 *      `<(...)`/`>(...)`, heredocs, output redirection `>`/`>>`,
 *      standalone `&` (background). Stderr-merge `2>&1` and
 *      `2>/dev/null` are tolerated.
 *   2. for_each_segment splits on top-level `&&`, `||`, `;`, `|`,
 *      `\n`, `\r` while respecting quotes. Backslash is a literal
 *      byte inside single quotes (POSIX) — only double-quoted
 *      content treats `\` as an escape.
 *   3. Each segment runs strip_neutral_prefix (`cd PATH`, `pushd PATH`,
 *      `popd`, `env VAR=val`, bare `VAR=val`) and the leftover leader
 *      determines its class.
 *
 * Per-segment classes:
 *   - READ/LIST/SEARCH: substantive. Stdin-readers (`cat`, `grep`,
 *     `head`, ...) downgrade to FORMAT when their argv carries no
 *     real file/source operand — the spec table encodes per-command
 *     short-flag-with-value letters and a min_operands threshold so
 *     `head -n 20`, `grep TODO`, `cat -n` reject correctly.
 *   - FORMAT: pipeline filter (`head` no file, `wc -l`, `sort -k 2`).
 *     Accepted only when preceded by `|` and the current statement
 *     already saw a producer; reset on `;`, `&&`, `||`, newline.
 *     `echo`/`printf`/`tr` are always FORMAT (their non-flag args
 *     are content, not files); `yes` is excluded entirely (unbounded
 *     output, no read-only use).
 *   - UNKNOWN: anything else, plus mutating writers detected here
 *     (`sed -i`, `awk -i inplace`, `awk '{system(...)}'`,
 *     `sed '...w out'`, `sort -o FILE`, `tree -o FILE`, `less -o`,
 *     `find -delete`/`-exec`/`-fprint*`, `fd --exec=`, `tee`, `xargs`
 *     wrapping a non-allowlisted command).
 *
 * Final verdict: at least one substantive segment AND every segment
 * classifies as one of the above (no UNKNOWN, no producer-less FORMAT).
 *
 * Known false-negative shapes accepted by design (bash will run them;
 * a real parser would catch each):
 *   - awk in-script redirection `'{print > "f"}'` or `'{print | cmd}'`
 *     (`>`/`|` are also operators, distinguishing needs awk grammar).
 *   - sed address-prefixed write where the boundary char is a digit
 *     (`1w out`, `2W out`) — the boundary heuristic uses non-alnum.
 *   - Subtle quoted-DSL behavior (`-e SCRIPT` separated long flags,
 *     `getline < file`, etc.).
 *   - `tail -f` runs forever; we don't detect `-f`. */
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

/* Format-only when used WITHOUT a real file operand (i.e. as a
 * pipeline filter). The promotion to CC_READ on file operand is
 * driven by `count_real_operands(cmd_spec_for(name), ...)` so flag
 * VALUES (`-n 20`, `-k 2`) don't masquerade as files; the
 * has_non_flag_operand fallback runs only for entries lacking a
 * spec.
 *
 * Deliberately excluded: `tee` (always writes its operand — falls
 * through to CC_UNKNOWN), `xargs` (runs an arbitrary wrapped command,
 * routed to classify_xargs which inspects the wrapped basename),
 * `yes` (unbounded output — no legitimate exploration use). */
static int format_filter_cmd(const char *t)
{
    static const char *const names[] = {
        "wc",  "sort", "uniq",   "cut",      "tr",    "awk",  "sed",  "head", "tail",   "tac",
        "rev", "fold", "expand", "unexpand", "paste", "comm", "join", "echo", "printf", "column",
    };
    return HAS(names, t);
}

/* Format filters whose non-flag arguments are content (echo/printf
 * strings, tr SETs), not files. Skipping them in the file-operand
 * promotion path keeps `echo hello`, `printf '%s' x`, and `tr a b`
 * classified as CC_FORMAT — and so rejected unless the pipeline pairs
 * them with a real source. */
static int pure_filter_cmd(const char *t)
{
    static const char *const names[] = {"echo", "printf", "tr"};
    return HAS(names, t);
}

/* Per-command spec for stdin-readers. `short_value` lists short-option
 * letters that consume the next token as their value (POSIX getopt's
 * required-argument shape). `min_operands` is the count of "real"
 * operands (after skipping flags AND their values) below which the
 * command would block on stdin or fail.
 *
 * Listed only for commands whose argv[0] alone is ambiguous about
 * whether stdin is needed: `cat`/`grep`/`head`/etc. classify as
 * exploration only when the argv shows a real file/source. Pure
 * directory commands (`ls`, `pwd`, `find`, `tree`) don't need an entry
 * — they have sensible no-arg behavior and aren't stdin readers.
 *
 * Long-flag-with-value awareness is intentionally limited to the
 * `--name=value` joined form (handled inline by skipping any `--`
 * token); `--name value` separated form would need a per-command
 * long-value table and the current set of false-positive shapes
 * (e.g. `grep --include FOO TODO`) is rare enough not to justify it. */
struct cmd_spec {
    const char *name;
    const char *short_value;
    int min_operands;
};

static const struct cmd_spec CMD_SPECS[] = {
    /* read_cmd group — all need at least one file. */
    {"cat", "", 1},
    {"less", "joP", 1},
    {"more", "n", 1},
    {"nl", "bdfhilnpsvw", 1},
    /* search_cmd group — grep family needs PATTERN + FILE; ripgrep et
     * al default to walking cwd, so pattern alone is enough. */
    {"grep", "ABCDdmef", 2},
    {"egrep", "ABCDdmef", 2},
    {"fgrep", "ABCDdmef", 2},
    {"rg", "ABCmtg", 1},
    {"ag", "ABCm", 1},
    {"ack", "ABCm", 1},
    /* format_filter_cmd group (excluding pure echo/printf/tr).
     * sed/awk need ≥2 because the script itself is a non-flag operand;
     * the file lives at position 2. */
    {"head", "nc", 1},
    {"tail", "nc", 1},
    {"wc", "", 1},
    {"sort", "kStTo", 1},
    {"uniq", "fsw", 1},
    {"cut", "bcdf", 1},
    /* sed/awk: short_value left empty on purpose. Their value-flags
     * (`-e SCRIPT`, `-f FILE`, `-v VAR=VAL`, awk's `-F SEP`) carry
     * operand-shaped data — the script/file is exactly the kind of
     * token we want to count toward min_operands. Treating them as
     * bool flags makes count_real_operands over-count slightly, which
     * is the safe direction (promotes to CC_READ when there's a real
     * source). The ≥2 minimum still rejects standalone `awk 'script'`
     * / `sed 'script'` since the script alone is one operand. */
    {"sed", "", 2},
    {"awk", "", 2},
    {"tac", "s", 1},
    {"rev", "", 1},
    {"fold", "w", 1},
    {"expand", "t", 1},
    {"unexpand", "t", 1},
    {"paste", "d", 1},
    {"comm", "", 1},
    {"join", "12teoav", 2},
    {"column", "csoN", 1},
};

static const struct cmd_spec *cmd_spec_for(const char *name)
{
    for (size_t i = 0; i < sizeof(CMD_SPECS) / sizeof(CMD_SPECS[0]); i++) {
        if (strcmp(CMD_SPECS[i].name, name) == 0)
            return &CMD_SPECS[i];
    }
    return NULL;
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
 * scanning. Newlines never reach take_word in normal flow because
 * for_each_segment splits on them, but treat them as terminators
 * defensively so a stray byte can't merge two words. */
static int is_word_end(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0';
}

/* Scan past whitespace in [s, end). */
static const char *skip_ws(const char *s, const char *end)
{
    while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
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
 * head/tail/wc segment is "format-only" or "read-from-file". */
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

/* Count "real" operands after the leader: tokens that are neither a
 * flag, nor a value consumed by a previous flag. Spec encodes which
 * short-option letters take a value:
 *   `-X` standalone where X is a value-taker → consume next token.
 *   `-XYZ` cluster where Y/Z is a value-taker but not last → the
 *      remainder of the cluster IS the value (no extra token consumed).
 *   `--name=value` → combined; no extra consume.
 *   `--name` separated long flag → assume bool (false positives go
 *      toward over-counting operands → over-acceptance, which is the
 *      same direction as the rest of the classifier).
 *
 * Caller passes the body INCLUDING the leader; we skip it internally. */
static int count_real_operands(const struct cmd_spec *spec, const char *body, const char *seg_end)
{
    const char *p = body;
    char *leader = take_word(p, seg_end, &p);
    free(leader);
    int count = 0;
    while (p < seg_end) {
        char *w = take_word(p, seg_end, &p);
        if (!w)
            break;
        if (w[0] == '-' && w[1] != '\0' && w[1] != '-') {
            /* Short-flag cluster. Find first value-taker char; if it
             * sits at the end of the cluster, the value is the next
             * token; otherwise the joined remainder is the value. */
            int consume_next = 0;
            for (size_t i = 1; w[i]; i++) {
                if (strchr(spec->short_value, w[i])) {
                    if (w[i + 1] == '\0')
                        consume_next = 1;
                    break;
                }
            }
            free(w);
            if (consume_next) {
                char *val = take_word(p, seg_end, &p);
                free(val);
            }
            continue;
        }
        if (w[0] == '-' && w[1] == '-' && w[2] != '\0') {
            /* Long flag (`--foo` or `--foo=bar`). Either way no extra
             * token consumed (separated long-with-value form would
             * over-count operands; tolerated, see comment above). */
            free(w);
            continue;
        }
        /* Bare `-` or `--` are operands (stdin marker / end-of-flags),
         * as is anything else. */
        free(w);
        count++;
    }
    return count;
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
            /* POSIX: backslash is literal inside single quotes — the
             * only way out of `'...'` is the matching `'`. Apply
             * escape handling only inside double quotes. Treating
             * `\'` as an escape would let `grep 'foo\' ; rm victim`
             * stay "in" the quote past the closing `'` and miss the
             * `;` separator entirely. */
            if (quote == '"' && c == '\\' && s[i + 1]) {
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
                i >= 1 && s[i - 1] == '2' && (i == 1 || strchr(" \t\n\r&|;", s[i - 2]) != NULL);
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

/* Sed write/execute commands anywhere in a script token. Looks for
 * `w`/`W`/`e` whose surrounding bytes mark it as a standalone command:
 *   - Right side: whitespace (followed by filename/cmd arg), end of
 *     token, `;`/newline (next sed command), `}` (close block).
 *   - Left side (after skipping optional whitespace): start of token,
 *     separator (`;`/newline), block (`{`/`}`), address terminator
 *     (`$`, `/`, ...). Encoded as "non-alphanumeric byte" — alnum
 *     means we're inside an identifier or regex content.
 * Catches `w out`, `1,$ w out`, `/pat/w out`, `s/x/y/;w out`,
 * `{w out;}`, the GNU `e` execute command, and the GNU `s/.../.../e`
 * eval flag (the `e` is the last byte of the token).
 *
 * Misses regex-content `w` (in `s/word/foo/` the `w` is followed by
 * `o`, not a boundary) and digit-prefixed addresses like `1w out` (the
 * `1` is alnum so the left-side boundary fails). Approximate by
 * design — see the top-of-file note. */
static int sed_script_writes(const char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c != 'w' && c != 'W' && c != 'e')
            continue;
        char next = s[i + 1];
        if (next != ' ' && next != '\t' && next != '\0' && next != ';' && next != '\n' &&
            next != '}')
            continue;
        size_t k = i;
        while (k > 0 && (s[k - 1] == ' ' || s[k - 1] == '\t'))
            k--;
        /* Reject left-side `-`: catches `-e`/`-w`/`-W` flag tokens
         * (which we should NOT classify as sed commands). The dash
         * also rules out `--exec` and friends — but those have a
         * non-boundary char (`x`, `c`) right after the letter so the
         * followed-by-boundary check above already filtered them. */
        if (k == 0 || (!isalnum((unsigned char)s[k - 1]) && s[k - 1] != '-'))
            return 1;
    }
    return 0;
}

/* Format-filter commands that, despite a file operand, are writing.
 * Called from the format-filter branch of classify_segment just before
 * a CC_READ promotion would have been returned.
 *
 * Two kinds of writes:
 *   - Flag-based (`sort -o FILE`, `awk -i inplace`): reliably detected
 *     by token inspection. (The sed `-i` form is handled separately,
 *     earlier in classify_segment, by sed_is_in_place — which also
 *     turns sed into UNKNOWN; this function is never reached for it.)
 *   - Script-body side effects (`awk '{system(...)}'`, `sed 'w out'`):
 *     best-effort substring scan, not a real parser. Catches common
 *     shapes; `awk '{print > "f"}'` and digit-prefixed sed addresses
 *     like `1w out` are accepted false negatives — see the top-of-
 *     file note. */
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
         * `-iinplace` (getopt-style required-argument cluster).
         *
         * Also scan every token for the substring `system(` — awk's
         * shell escape hatch. Reliable in practice: awk comments
         * start with `#` and `system(` is unusual in regex literals. */
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
            int hit = (w[0] == '-' && w[1] == 'i' && strcmp(w + 2, "inplace") == 0) ||
                      strstr(w, "system(") != NULL;
            free(w);
            if (hit)
                return 1;
        }
    } else if (strcmp(cmd, "sed") == 0) {
        while (p < seg_end) {
            char *w = take_word(p, seg_end, &p);
            if (!w)
                return 0;
            int hit = sed_script_writes(w);
            free(w);
            if (hit)
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
        /* Stdin-readers (cat/grep/less/...) need a real file/source.
         * Without one — `cat`, `grep TODO`, `head -n 20` — the command
         * blocks waiting on stdin. Downgrade to CC_FORMAT: walk_cb
         * accepts CC_FORMAT only inside a downstream pipeline (after
         * `|`), so standalone these reject and fall through to the
         * verbose preview. */
        const struct cmd_spec *spec = cmd_spec_for(w1);
        if (spec && count_real_operands(spec, body, seg_end) < spec->min_operands) {
            free(w1);
            free(w2);
            return CC_FORMAT;
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
     * with a file operand.
     *
     * Pure filters (echo/printf/tr) never promote: their non-flag args
     * are content, not files. Script filters (sed/awk) and ordinary
     * file filters share one path via the spec's min_operands. */
    if (format_filter_cmd(w1)) {
        if (pure_filter_cmd(w1)) {
            free(w1);
            free(w2);
            return CC_FORMAT;
        }
        const struct cmd_spec *spec = cmd_spec_for(w1);
        int has_file;
        if (spec) {
            has_file = count_real_operands(spec, body, seg_end) >= spec->min_operands;
        } else {
            /* Defensive fallback for any format_filter_cmd entry that
             * lacks a spec — treat any non-flag token as a file. */
            const char *rest = body;
            char *consume = take_word(rest, seg_end, &rest);
            free(consume);
            has_file = has_non_flag_operand(rest, seg_end);
        }
        if (has_file) {
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

/* Connector type that *preceded* a segment. Used by walk_cb to gate
 * CC_FORMAT acceptance: a format-only segment is meaningful only when
 * some upstream segment in the same pipeline produced data for it.
 * Statement-level connectors (`;`, `&&`, `||`, newlines, start-of-
 * input) reset that producer state. */
enum cmd_sep {
    SEP_NONE,  /* start of input */
    SEP_PIPE,  /* | */
    SEP_OTHER, /* ;, &&, ||, \n, \r */
};

typedef int (*seg_cb_t)(const char *seg, const char *end, enum cmd_sep prev_sep, void *user);

/* Walk `s`, splitting on top-level connectors while respecting quotes.
 * Each segment's preceding connector is forwarded to the callback so
 * it can distinguish "downstream of `|`" from "start of new statement".
 * Stops early and returns 0 if cb returns 0; returns 1 on full walk. */
static int for_each_segment(const char *s, seg_cb_t cb, void *user)
{
    size_t n = strlen(s);
    const char *end = s + n;
    const char *seg = s;
    enum cmd_sep prev_sep = SEP_NONE;
    char quote = 0;
    for (size_t i = 0; i < n;) {
        char c = s[i];
        if (quote) {
            /* Backslash escape only inside double quotes (see
             * has_disqualifier for why single quotes are literal). */
            if (quote == '"' && c == '\\' && s[i + 1])
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
        enum cmd_sep this_sep = SEP_NONE;
        if ((c == '&' && s[i + 1] == '&') || (c == '|' && s[i + 1] == '|')) {
            split_len = 2;
            this_sep = SEP_OTHER;
        } else if (c == ';' || c == '\n' || c == '\r') {
            split_len = 1;
            this_sep = SEP_OTHER;
        } else if (c == '|') {
            split_len = 1;
            this_sep = SEP_PIPE;
        }
        if (split_len) {
            if (!cb(seg, s + i, prev_sep, user))
                return 0;
            prev_sep = this_sep;
            seg = s + i + split_len;
            i += (size_t)split_len;
            continue;
        }
        i++;
    }
    return cb(seg, end, prev_sep, user);
}

struct walk_state {
    int verdict;                /* 1 = still exploration, 0 = give up */
    int saw_substantive;        /* observed at least one CC_READ/LIST/SEARCH anywhere */
    int statement_has_producer; /* current statement saw a producer; reset on ;/&&/|| */
};

static int walk_cb(const char *seg, const char *end, enum cmd_sep prev_sep, void *user)
{
    struct walk_state *ws = user;

    /* Statement boundary: `;`, `&&`, `||`, newline, or start of input
     * starts a fresh "statement" whose CC_FORMAT segments need their
     * own upstream producer. A `|` continues the current statement.
     * Apply BEFORE the empty-segment skip so a chain like `ls; ; sort`
     * still resets the producer for `sort` even though the middle
     * empty segment is skipped — without this, the empty piece would
     * swallow the `;` connector context and the producer state would
     * leak past the statement boundary. */
    if (prev_sep != SEP_PIPE)
        ws->statement_has_producer = 0;

    /* Skip whitespace-only segments. Trailing newlines, `\r\n` line
     * endings, and adjacent connectors (`cmd ;\n`, `cmd &&` with
     * trailing whitespace) all produce empty splits. We don't try to
     * distinguish shell-valid no-ops from syntax errors like `;;` or
     * `ls &&` — a malformed command fails at the shell, the error
     * reaches the model verbatim via the bash tool's captured output,
     * and the user-visible silent header is correct either way. */
    const char *p = skip_ws(seg, end);
    if (p >= end)
        return 1;

    enum cmd_class c = classify_segment(seg, end);
    if (c == CC_UNKNOWN) {
        ws->verdict = 0;
        return 0;
    }
    if (c == CC_FORMAT) {
        /* Filter without an upstream producer in this statement would
         * block on stdin or emit unrelated content. Reject so e.g.
         * `ls; sort`, `cat` standalone, `grep x f || printf '...'`
         * fall through to verbose preview. */
        if (!ws->statement_has_producer) {
            ws->verdict = 0;
            return 0;
        }
        return 1;
    }
    /* Substantive segment (CC_READ/LIST/SEARCH). */
    ws->statement_has_producer = 1;
    ws->saw_substantive = 1;
    return 1;
}

int bash_cmd_is_exploration(const char *cmd)
{
    if (!cmd)
        return 0;
    /* Empty / pure-whitespace input has nothing to classify — reject so
     * the caller falls through to the verbose preview. */
    const char *p = cmd;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (!*p)
        return 0;
    if (has_disqualifier(cmd))
        return 0;
    struct walk_state ws = {.verdict = 1, .saw_substantive = 0, .statement_has_producer = 0};
    for_each_segment(cmd, walk_cb, &ws);
    return ws.verdict && ws.saw_substantive;
}
