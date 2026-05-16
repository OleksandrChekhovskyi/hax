/* SPDX-License-Identifier: MIT */
#include "tools/bash_cd_strip.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

/* Path-safe character: one of the chars an LLM emits in a directory
 * token without introducing shell semantics. Restrictive on purpose —
 * if we see anything else (glob, escape, command substitution), we
 * bail rather than risk a wrong rewrite. */
static int is_path_safe(char c)
{
    unsigned char u = (unsigned char)c;
    if (u >= 0x80) /* non-ASCII bytes — UTF-8 path components are common */
        return 1;
    if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9'))
        return 1;
    return c == '/' || c == '.' || c == '_' || c == '-' || c == '+';
}

/* Whether a byte inside a parsed cd-target token is acceptable as
 * part of the resolved path. The acceptable set widens with quoting:
 *
 *   - quoting == 0 (unquoted): bash applies word splitting on IFS
 *     whitespace and pathname expansion on glob meta to anything
 *     unquoted, so the literal portion must be path-safe too —
 *     no whitespace, no `*`/`?`/`[`, no shell metachars.
 *
 *   - quoting == 1 (double-quoted): word splitting and globbing
 *     don't fire inside `"..."`. Spaces, glob meta, parens, etc.
 *     are literal; only `$` (parameter / command substitution) and
 *     backtick remain dangerous. (Backslash is already rejected at
 *     tokenize time.)
 *
 *   - quoting == 2 (single-quoted) bypasses this entirely; its
 *     branch in resolve_cd_target copies the contents verbatim. */
static int is_token_safe(char c, int quoting)
{
    if (quoting == 1)
        return c != '$' && c != '`';
    return is_path_safe(c);
}

/* Whether a value is safe to substitute into an unquoted shell
 * context. Bash word-splits unquoted parameter-expansion results on
 * IFS whitespace and applies pathname expansion (glob meta) to every
 * resulting word — so a space or `*` in $HOME makes the original
 * `cd $HOME/proj` parse as multiple args, or expand to a different
 * path than the literal value. Either way it isn't equivalent to
 * "treat the value as a single path", and stripping would change
 * execution semantics. Tilde results escape word splitting (POSIX
 * 2.6.5 limits it to parameter/command/arithmetic expansion) but
 * pathname expansion is per-word and the bash manual doesn't carve
 * them out, so we apply the same conservative check there too.
 * Quoted contexts ("$VAR", '~/x') bypass both steps and don't need
 * this. */
static int unquoted_expansion_safe(const char *s)
{
    for (; *s; s++) {
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '*' || *s == '?' || *s == '[')
            return 0;
    }
    return 1;
}

/* Compare two paths ignoring any trailing slash on either side (so
 * "/foo" == "/foo/"). The cwd from getcwd() never has a trailing slash
 * except at root ("/"), so we preserve a lone "/" by keeping at least
 * one character on each side. */
static int paths_equal(const char *a, size_t al, const char *b, size_t bl)
{
    while (al > 1 && a[al - 1] == '/')
        al--;
    while (bl > 1 && b[bl - 1] == '/')
        bl--;
    return al == bl && memcmp(a, b, al) == 0;
}

/* Resolve a parsed cd target to an absolute path string (caller frees),
 * or return NULL if the token is too fancy to handle safely. Quoting:
 *   0 = unquoted    — supports leading ~, ~/, $HOME, ${HOME}, bare "."
 *   1 = "double"    — supports leading $HOME, ${HOME}
 *                     (bash leaves ~ literal inside double quotes)
 *   2 = 'single'    — strict literal, no expansion
 *
 * The portion of the token *after* a recognized prefix must be all
 * path-safe characters; that's how we keep `$HOMEx`, `$(...)`, globs,
 * and similar shell-significant input from sneaking past as a "literal
 * path with a leading var". */
static char *resolve_cd_target(const char *tok, size_t tok_len, int quoting, const char *cwd,
                               const char *home)
{
    /* `cd .` is a cwd alias in any quoting — single quotes don't change
     * the meaning of a bare `.` and there's no expansion to suppress.
     * Checked before the single-quoted branch so `cd '.'` matches too. */
    if (tok_len == 1 && tok[0] == '.')
        return xstrdup(cwd);

    /* Single-quoted: bash performs no expansion at all. */
    if (quoting == 2) {
        char *out = xmalloc(tok_len + 1);
        memcpy(out, tok, tok_len);
        out[tok_len] = '\0';
        return out;
    }

    /* Tilde expansion — bash only honors a leading `~` when unquoted. */
    if (quoting == 0 && tok_len >= 1 && tok[0] == '~') {
        if (!home || !*home)
            return NULL;
        if (!unquoted_expansion_safe(home))
            return NULL;
        if (tok_len == 1)
            return xstrdup(home);
        if (tok[1] != '/') /* `~user/...` would need getpwnam — bail */
            return NULL;
        for (size_t i = 2; i < tok_len; i++) {
            if (!is_path_safe(tok[i]))
                return NULL;
        }
        return xasprintf("%s%.*s", home, (int)(tok_len - 1), tok + 1);
    }

    /* $HOME / ${HOME} at the start of the token. The boundary check
     * after the bare-name form prevents us from misreading $HOMEx as
     * $HOME + "x" — bash would parse that as the (typically unset)
     * variable $HOMEx. */
    const char *var_val = NULL;
    size_t consumed = 0;
    if (tok_len >= 5 && memcmp(tok, "$HOME", 5) == 0 && (tok_len == 5 || tok[5] == '/')) {
        var_val = home;
        consumed = 5;
    } else if (tok_len >= 7 && memcmp(tok, "${HOME}", 7) == 0) {
        var_val = home;
        consumed = 7;
    }

    if (var_val) {
        if (!*var_val)
            return NULL;
        if (quoting == 0 && !unquoted_expansion_safe(var_val))
            return NULL;
        for (size_t i = consumed; i < tok_len; i++) {
            if (!is_token_safe(tok[i], quoting))
                return NULL;
        }
        return xasprintf("%s%.*s", var_val, (int)(tok_len - consumed), tok + consumed);
    }

    /* No variable / tilde. Treat as a literal path only when it's
     * absolute and made entirely of token-safe characters. Relative
     * paths could only match cwd by accident (and only when the agent
     * happens to be at their parent), so we don't try. */
    if (tok_len < 1 || tok[0] != '/')
        return NULL;
    for (size_t i = 0; i < tok_len; i++) {
        if (!is_token_safe(tok[i], quoting))
            return NULL;
    }
    char *out = xmalloc(tok_len + 1);
    memcpy(out, tok, tok_len);
    out[tok_len] = '\0';
    return out;
}

size_t bash_strip_cd_prefix(const char *cmd, const char *cwd, const char *home)
{
    if (!cmd || !cwd)
        return 0;

    const char *p = cmd;
    while (*p == ' ' || *p == '\t')
        p++;

    /* `cd` keyword followed by at least one whitespace char. We don't
     * accept `cd\n…` etc. because LLMs don't emit those, and accepting
     * them would force us to track newlines through the rest of the
     * parser. */
    if (p[0] != 'c' || p[1] != 'd' || (p[2] != ' ' && p[2] != '\t'))
        return 0;
    p += 2;
    while (*p == ' ' || *p == '\t')
        p++;

    /* Parse exactly one shell word. Reject mixed quoting like
     * `cd "/a"'/b'` and any escape sequence — both are signals the
     * model is doing something we shouldn't second-guess. */
    const char *tok;
    size_t tok_len;
    int quoting;
    if (*p == '\'') {
        quoting = 2;
        p++;
        tok = p;
        while (*p && *p != '\'')
            p++;
        if (*p != '\'')
            return 0;
        tok_len = (size_t)(p - tok);
        p++;
    } else if (*p == '"') {
        quoting = 1;
        p++;
        tok = p;
        while (*p && *p != '"') {
            if (*p == '\\') /* backslash escapes — bail rather than interpret */
                return 0;
            p++;
        }
        if (*p != '"')
            return 0;
        tok_len = (size_t)(p - tok);
        p++;
    } else {
        quoting = 0;
        tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ';' && *p != '&' && *p != '|' && *p != '<' &&
               *p != '>' && *p != '(' && *p != ')' && *p != '`' && *p != '\\' && *p != '"' &&
               *p != '\'')
            p++;
        tok_len = (size_t)(p - tok);
        if (tok_len == 0)
            return 0;
    }
    /* Reject quoted-then-anything (e.g. `cd "/a"foo`) — that's word
     * concatenation in bash, which we don't model. */
    if (quoting != 0 && *p != ' ' && *p != '\t' && *p != '&' && *p != ';')
        return 0;

    while (*p == ' ' || *p == '\t')
        p++;
    if (p[0] != '&' || p[1] != '&')
        return 0;
    p += 2;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0') /* `cd X &&` with no follow-up — nothing to keep */
        return 0;

    char *resolved = resolve_cd_target(tok, tok_len, quoting, cwd, home);
    if (!resolved)
        return 0;
    int eq = paths_equal(resolved, strlen(resolved), cwd, strlen(cwd));
    free(resolved);
    return eq ? (size_t)(p - cmd) : 0;
}
