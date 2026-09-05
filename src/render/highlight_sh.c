/* SPDX-License-Identifier: MIT */
#include "render/highlight_sh.h"

#include <stddef.h>
#include <strings.h>

static int is_operator(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '>' || c == '<' || c == '(' || c == ')';
}

/* POSIX reserves '#' as a comment only at the start of a word. */
static int is_comment_start(char prev)
{
    return prev == 0 || prev == ' ' || prev == '\t' || prev == '\n' || prev == ';' || prev == '&' ||
           prev == '|' || prev == '(';
}

static int is_info_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

void highlight_sh(const char *s, size_t n, sh_span_fn emit, void *user)
{
    enum sh_span_kind cur = SH_SPAN_PLAIN;
    size_t run = 0;
    char quote = 0;
    int escape = 0;
    int comment = 0;
    char prev = 0;

    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        enum sh_span_kind kind;
        if (escape) {
            escape = 0;
            kind = quote ? SH_SPAN_STRING : SH_SPAN_PLAIN;
        } else if (quote == '\'') {
            kind = SH_SPAN_STRING;
            if (c == '\'')
                quote = 0;
        } else if (quote == '"') {
            kind = SH_SPAN_STRING;
            if (c == '\\')
                escape = 1;
            else if (c == '"')
                quote = 0;
        } else if (comment) {
            if (c == '\n')
                comment = 0;
            kind = comment ? SH_SPAN_COMMENT : SH_SPAN_PLAIN;
        } else if (c == '\\') {
            escape = 1;
            kind = SH_SPAN_PLAIN;
        } else if (c == '\'' || c == '"') {
            quote = c;
            kind = SH_SPAN_STRING;
        } else if (c == '#' && is_comment_start(prev)) {
            comment = 1;
            kind = SH_SPAN_COMMENT;
        } else if (is_operator(c)) {
            kind = SH_SPAN_OPERATOR;
        } else {
            kind = SH_SPAN_PLAIN;
        }
        if (kind != cur) {
            if (i > run)
                emit(s + run, i - run, cur, user);
            run = i;
            cur = kind;
        }
        prev = c;
    }
    if (n > run)
        emit(s + run, n - run, cur, user);
}

int highlight_sh_lang(const char *info, size_t len)
{
    size_t i = 0;
    while (i < len && is_info_space(info[i]))
        i++;
    size_t start = i;
    while (i < len && !is_info_space(info[i]) && info[i] != '\n')
        i++;
    size_t word = i - start;
    return (word == 2 && strncasecmp(info + start, "sh", 2) == 0) ||
           (word == 4 && strncasecmp(info + start, "bash", 4) == 0) ||
           (word == 5 && strncasecmp(info + start, "shell", 5) == 0);
}
