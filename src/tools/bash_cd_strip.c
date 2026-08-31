/* SPDX-License-Identifier: MIT */
#include "tools/bash_cd_strip.h"

#include <stdlib.h>
#include <string.h>

#include "xalloc.h"

enum quote_mode {
    QUOTE_NONE,
    QUOTE_DOUBLE,
    QUOTE_SINGLE,
};

/* Restrict unquoted bytes to characters that cannot introduce shell semantics. */
static int is_path_safe(char c)
{
    unsigned char u = (unsigned char)c;
    if (u >= 0x80) /* non-ASCII bytes — UTF-8 path components are common */
        return 1;
    if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9'))
        return 1;
    return c == '/' || c == '.' || c == '_' || c == '-' || c == '+';
}

/* Double quotes permit literal path bytes except the remaining expansion operators. */
static int is_token_safe(char c, enum quote_mode quote)
{
    if (quote == QUOTE_DOUBLE)
        return c != '$' && c != '`';
    return is_path_safe(c);
}

/* Unquoted expansion values must survive word splitting and pathname expansion unchanged. */
static int unquoted_expansion_safe(const char *value)
{
    for (; *value; value++) {
        if (*value == ' ' || *value == '\t' || *value == '\n' || *value == '*' || *value == '?' ||
            *value == '[')
            return 0;
    }
    return 1;
}

/* Ignore trailing slashes without reducing the root path to an empty string. */
static int paths_equal(const char *a, size_t a_len, const char *b, size_t b_len)
{
    while (a_len > 1 && a[a_len - 1] == '/')
        a_len--;
    while (b_len > 1 && b[b_len - 1] == '/')
        b_len--;
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

/* Return an owned absolute path, or NULL for shell syntax not modeled here. */
static char *resolve_cd_target(const char *token, size_t token_len, enum quote_mode quote,
                               const char *cwd, const char *home)
{
    /* Dot aliases cwd regardless of quoting. */
    if (token_len == 1 && token[0] == '.')
        return xstrdup(cwd);

    if (quote == QUOTE_SINGLE) {
        char *path = xmalloc(token_len + 1);
        memcpy(path, token, token_len);
        path[token_len] = '\0';
        return path;
    }

    /* Shells do not expand a quoted tilde. */
    if (quote == QUOTE_NONE && token_len >= 1 && token[0] == '~') {
        if (!home || !*home)
            return NULL;
        if (!unquoted_expansion_safe(home))
            return NULL;
        if (token_len == 1)
            return xstrdup(home);
        if (token[1] != '/') /* `~user/...` would need getpwnam — bail */
            return NULL;
        for (size_t i = 2; i < token_len; i++) {
            if (!is_path_safe(token[i]))
                return NULL;
        }
        return xasprintf("%s%.*s", home, (int)(token_len - 1), token + 1);
    }

    /* The slash boundary distinguishes $HOME/path from the variable $HOMEx. */
    const char *expansion = NULL;
    size_t prefix_len = 0;
    if (token_len >= 5 && memcmp(token, "$HOME", 5) == 0 && (token_len == 5 || token[5] == '/')) {
        expansion = home;
        prefix_len = 5;
    } else if (token_len >= 7 && memcmp(token, "${HOME}", 7) == 0) {
        expansion = home;
        prefix_len = 7;
    }

    if (expansion) {
        if (!*expansion)
            return NULL;
        if (quote == QUOTE_NONE && !unquoted_expansion_safe(expansion))
            return NULL;
        for (size_t i = prefix_len; i < token_len; i++) {
            if (!is_token_safe(token[i], quote))
                return NULL;
        }
        return xasprintf("%s%.*s", expansion, (int)(token_len - prefix_len), token + prefix_len);
    }

    /* Resolving relative paths would require filesystem state, so accept only absolute literals. */
    if (token_len < 1 || token[0] != '/')
        return NULL;
    for (size_t i = 0; i < token_len; i++) {
        if (!is_token_safe(token[i], quote))
            return NULL;
    }
    char *path = xmalloc(token_len + 1);
    memcpy(path, token, token_len);
    path[token_len] = '\0';
    return path;
}

size_t bash_strip_cd_prefix(const char *command, const char *cwd, const char *home)
{
    if (!command || !cwd)
        return 0;

    const char *cursor = command;
    while (*cursor == ' ' || *cursor == '\t')
        cursor++;

    /* Accept only horizontal whitespace to keep the grammar on one command line. */
    if (cursor[0] != 'c' || cursor[1] != 'd' || (cursor[2] != ' ' && cursor[2] != '\t'))
        return 0;
    cursor += 2;
    while (*cursor == ' ' || *cursor == '\t')
        cursor++;

    /* Mixed quoting and escapes are outside this conservative grammar. */
    const char *token;
    size_t token_len;
    enum quote_mode quote;
    if (*cursor == '\'') {
        quote = QUOTE_SINGLE;
        cursor++;
        token = cursor;
        while (*cursor && *cursor != '\'')
            cursor++;
        if (*cursor != '\'')
            return 0;
        token_len = (size_t)(cursor - token);
        cursor++;
    } else if (*cursor == '"') {
        quote = QUOTE_DOUBLE;
        cursor++;
        token = cursor;
        while (*cursor && *cursor != '"') {
            if (*cursor == '\\') /* backslash escapes — bail rather than interpret */
                return 0;
            cursor++;
        }
        if (*cursor != '"')
            return 0;
        token_len = (size_t)(cursor - token);
        cursor++;
    } else {
        quote = QUOTE_NONE;
        token = cursor;
        while (*cursor && *cursor != ' ' && *cursor != '\t' && *cursor != ';' && *cursor != '&' &&
               *cursor != '|' && *cursor != '<' && *cursor != '>' && *cursor != '(' &&
               *cursor != ')' && *cursor != '`' && *cursor != '\\' && *cursor != '"' &&
               *cursor != '\'')
            cursor++;
        token_len = (size_t)(cursor - token);
        if (token_len == 0)
            return 0;
    }
    /* Reject quoted-then-anything (e.g. `cd "/a"foo`) — that's word
     * concatenation in bash, which we don't model. */
    if (quote != QUOTE_NONE && *cursor != ' ' && *cursor != '\t' && *cursor != '&' &&
        *cursor != ';')
        return 0;

    while (*cursor == ' ' || *cursor == '\t')
        cursor++;
    if (cursor[0] != '&' || cursor[1] != '&')
        return 0;
    cursor += 2;
    while (*cursor == ' ' || *cursor == '\t')
        cursor++;
    if (*cursor == '\0') /* `cd X &&` with no follow-up — nothing to keep */
        return 0;

    char *resolved_path = resolve_cd_target(token, token_len, quote, cwd, home);
    if (!resolved_path)
        return 0;
    int matches_cwd = paths_equal(resolved_path, strlen(resolved_path), cwd, strlen(cwd));
    free(resolved_path);
    return matches_cwd ? (size_t)(cursor - command) : 0;
}
