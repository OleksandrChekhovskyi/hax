/* SPDX-License-Identifier: MIT */
#ifndef HAX_UTIL_H
#define HAX_UTIL_H

#include <stdarg.h>
#include <stddef.h>

/* Set LC_CTYPE, and only LC_CTYPE, to a UTF-8 locale: LC_NUMERIC under the user's locale can put a
 * decimal comma in serialized JSON. Publishes the choice to the environment unless a non-UTF-8
 * LC_ALL is pinned there. Call before other initialization, and before any thread reads the
 * environment. */
void locale_init_utf8(void);
/* Return whether this process can decode and measure multibyte text. */
int locale_have_utf8(void);
/* Return the LC_CTYPE a child must be given to read what hax renders, or NULL when the environment
 * already supplies one — the usual case — or when no UTF-8 locale exists to give. Only a pinned
 * LC_ALL, which outranks the published LC_CTYPE, makes this necessary. Valid until the next
 * setlocale(). */
const char *locale_child_ctype_override(void);

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

/* Owned concatenation of two NULL-terminated string arrays (either may be NULL), or NULL when
 * the result would be empty. Free with string_array_free. */
char **string_array_concat(const char *const *first, const char *const *second);

/* Return a newly allocated shell-safe, single-quoted copy. NULL becomes empty. */
char *shell_single_quote(const char *str);

/* Parse a complete base-10 integer into out. Returns 1 on success and 0 otherwise. */
int parse_int(const char *str, int *out);

/* CLOCK_MONOTONIC milliseconds since an unspecified epoch. */
long monotonic_ms(void);

/* Round to seconds and format compactly, omitting zero remainders ("10m", "2h"); non-positive
 * values produce "0s". */
void format_duration(char *out, size_t out_size, long duration_ms);
/* As format_duration, but zero remainders stay ("10m 00s"), so a display that repaints in
 * place never shrinks and regrows at a unit boundary while ticking. */
void format_duration_steady(char *out, size_t out_size, long duration_ms);
/* Use more decimal places for sub-dollar values; non-positive values produce "$0.00". */
void format_cost(char *out, size_t out_size, double usd);

/* Fill `out` with `len` bytes from the system entropy source. Aborts on entropy failure. */
void random_bytes(void *out, size_t len);

/* Write a lowercase UUIDv4 (36 bytes plus the NUL terminator). Aborts on entropy failure. */
void gen_uuid_v4(char out[37]);

#endif /* HAX_UTIL_H */
