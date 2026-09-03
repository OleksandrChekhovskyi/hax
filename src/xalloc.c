/* SPDX-License-Identifier: MIT */
#include "xalloc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void die_oom(void)
{
    fprintf(stderr, "hax: out of memory\n");
    abort();
}

void *xmalloc(size_t size)
{
    void *result = malloc(size ? size : 1);
    if (!result)
        die_oom();
    return result;
}

void *xcalloc(size_t count, size_t element_size)
{
    void *result = (count && element_size) ? calloc(count, element_size) : calloc(1, 1);
    if (!result)
        die_oom();
    return result;
}

void *xrealloc(void *ptr, size_t size)
{
    void *result = realloc(ptr, size ? size : 1);
    if (!result)
        die_oom();
    return result;
}

char *xstrdup(const char *str)
{
    if (!str)
        return NULL;
    char *result = strdup(str);
    if (!result)
        die_oom();
    return result;
}

char *xvasprintf(const char *format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0)
        return NULL;

    char *result = xmalloc((size_t)length + 1);
    va_copy(copy, args);
    int written = vsnprintf(result, (size_t)length + 1, format, copy);
    va_end(copy);
    if (written < 0 || written > length) {
        free(result);
        return NULL;
    }
    return result;
}

char *xasprintf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char *result = xvasprintf(format, args);
    va_end(args);
    return result;
}

void string_array_free(char **strings)
{
    if (!strings)
        return;
    for (char **string = strings; *string; string++)
        free(*string);
    free(strings);
}

size_t string_array_count(const char *const *strings)
{
    size_t count = 0;
    for (const char *const *string = strings; string && *string; string++)
        count++;
    return count;
}

char **string_array_concat(const char *const *first, const char *const *second)
{
    size_t n_strings = string_array_count(first) + string_array_count(second);
    if (n_strings == 0)
        return NULL;

    char **combined = xmalloc(sizeof(*combined) * (n_strings + 1));
    size_t n = 0;
    for (const char *const *string = first; string && *string; string++)
        combined[n++] = xstrdup(*string);
    for (const char *const *string = second; string && *string; string++)
        combined[n++] = xstrdup(*string);
    combined[n] = NULL;
    return combined;
}
