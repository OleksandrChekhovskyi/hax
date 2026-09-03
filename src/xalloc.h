/* SPDX-License-Identifier: MIT */
#ifndef HAX_XALLOC_H
#define HAX_XALLOC_H

#include <stdarg.h>
#include <stddef.h>

/* Allocation failures are fatal. Zero-sized allocation requests return non-NULL. */
void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t element_size);
void *xrealloc(void *ptr, size_t size);
/* Return an allocated duplicate, or NULL for NULL input. */
char *xstrdup(const char *str);
/* Return an allocated formatted string, or NULL if formatting fails. */
char *xasprintf(const char *format, ...) __attribute__((format(printf, 1, 2), nonnull(1)));
/* As xasprintf(), without consuming or ending args. */
char *xvasprintf(const char *format, va_list args)
    __attribute__((format(printf, 1, 0), nonnull(1)));
/* Print the OOM diagnostic and abort. The allocation wrappers and growable buffers share it. */
void die_oom(void);

/* Free a NULL-terminated array and its strings. NULL-safe. */
void string_array_free(char **strings);

/* Number of strings before the terminator; 0 for NULL. */
size_t string_array_count(const char *const *strings);

/* Owned concatenation of two NULL-terminated string arrays (either may be NULL), or NULL when
 * the result would be empty. Free with string_array_free. */
char **string_array_concat(const char *const *first, const char *const *second);

#endif /* HAX_XALLOC_H */
