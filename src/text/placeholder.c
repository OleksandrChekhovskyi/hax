/* SPDX-License-Identifier: MIT */
#include "text/placeholder.h"

#include <stdlib.h>
#include <string.h>

#include "xalloc.h"

static char *placeholder_token(const char *name)
{
    return xasprintf("{%s}", name);
}

int placeholder_present(const char *text, const char *name)
{
    char *token = placeholder_token(name);
    int present = strstr(text, token) != NULL;
    free(token);
    return present;
}

char *placeholder_expand(const char *text, const char *name, const char *value)
{
    char *token = placeholder_token(name);
    size_t token_len = strlen(token);
    size_t value_len = strlen(value);
    size_t occurrences = 0;
    for (const char *at = strstr(text, token); at; at = strstr(at + token_len, token))
        occurrences++;

    char *result = xmalloc(strlen(text) + occurrences * value_len + 1);
    char *out = result;
    const char *rest = text;
    for (const char *at = strstr(rest, token); at; at = strstr(rest, token)) {
        memcpy(out, rest, (size_t)(at - rest));
        out += at - rest;
        memcpy(out, value, value_len);
        out += value_len;
        rest = at + token_len;
    }
    strcpy(out, rest);
    free(token);
    return result;
}
