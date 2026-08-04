/* SPDX-License-Identifier: MIT */
#ifndef HAX_UTIL_H
#define HAX_UTIL_H

#include <stdarg.h>
#include <stddef.h>

/* Set only LC_CTYPE to a UTF-8 locale. Changing LC_ALL would make numeric formatting depend on the
 * user's locale and could produce invalid JSON. Call before other initialization. */
void locale_init_utf8(void);
/* Return whether UTF-8 LC_CTYPE initialization succeeded. */
int locale_have_utf8(void);

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

/* Free a NULL-terminated array and its strings. NULL-safe. */
void string_array_free(char **strings);

/* Return a newly allocated shell-safe, single-quoted copy. NULL becomes empty. */
char *shell_single_quote(const char *str);

/* Emit one `hax: <message>` line to stderr, without changing control flow. Callers supply no prefix
 * or newline. hax_err uses the error color and hax_warn the warning color on terminals. */
__attribute__((format(printf, 1, 2))) void hax_err(const char *format, ...);
__attribute__((format(printf, 1, 2))) void hax_warn(const char *format, ...);

/* Monotonic count of completed hax_err() and hax_warn() writes. */
unsigned long hax_diag_sequence(void);

/* Return 0 for a regular file, or -1 with errno set for any other path. */
int ensure_regular_file(const char *path);

/* Open a regular file for reading without blocking on special files. The caller owns the returned
 * descriptor. Returns -1 with errno set on failure or when the path is not a regular file. */
int open_regular_file(const char *path);

/* Return newly allocated, NUL-terminated file contents, or NULL with errno set. */
char *slurp_file(const char *path, size_t *out_len);

/* Read at most cap bytes. On success, optional outputs report the returned length and whether more
 * data exists. The allocation grows with the bytes read rather than cap. Returns NULL with errno
 * set on failure. */
char *slurp_file_capped(const char *path, size_t cap, size_t *out_len, int *out_truncated);

/* Write exactly length bytes, retrying interrupted and short writes. Returns 0 on success or -1
 * with errno set. */
int write_all(int fd, const void *data, size_t length);

/* Return an allocated `<base>/hax/<relative_path>`, using the named non-empty XDG base or the HOME
 * fallback. Return NULL when neither base is available. */
char *xdg_hax_config_path(const char *relative_path); /* XDG_CONFIG_HOME or HOME/.config */
char *xdg_hax_state_path(const char *relative_path);  /* XDG_STATE_HOME or HOME/.local/state */
char *xdg_hax_cache_path(const char *relative_path);  /* XDG_CACHE_HOME or HOME/.cache */

/* Return a copy with all trailing slashes removed. */
char *dup_trim_trailing_slash(const char *str);

/* Parse positive byte counts with optional case-insensitive k/m binary suffixes. */
long parse_size(const char *str);

/* Parse a non-negative duration with an optional ms/s/m/h suffix and return milliseconds. A missing
 * suffix means seconds. Returns -1 for invalid input. */
long parse_duration_ms(const char *str);

/* Parse a complete base-10 integer into out. Returns 1 on success and 0 otherwise. */
int parse_int(const char *str, int *out);

/* CLOCK_MONOTONIC milliseconds since an unspecified epoch. */
long monotonic_ms(void);

/* Use binary k/M suffixes for token counts; negative values produce "?". */
void format_tokens(char *out, size_t out_size, long tokens);
/* Round to seconds and format compactly, omitting zero remainders ("10m", "2h"); non-positive
 * values produce "0s". */
void format_duration(char *out, size_t out_size, long duration_ms);
/* As format_duration, but zero remainders stay ("10m 00s"), so a display that repaints in
 * place never shrinks and regrows at a unit boundary while ticking. */
void format_duration_steady(char *out, size_t out_size, long duration_ms);
/* Use more decimal places for sub-dollar values; non-positive values produce "$0.00". */
void format_cost(char *out, size_t out_size, double usd);
/* Include the usage percentage when context_limit is positive. */
void format_context(char *out, size_t out_size, long context_tokens, long context_limit);

#define COST_DISPLAY_MIN 0.00005

/* Write a lowercase UUIDv4 (36 bytes plus the NUL terminator). Aborts on entropy failure. */
void gen_uuid_v4(char out[37]);

/* Replace each line suffix beyond max_line_bytes with an elision marker while preserving newline
 * structure. Returns a newly allocated NUL-terminated buffer and stores its length in out_len. */
char *cap_line_lengths(const char *data, size_t length, size_t max_line_bytes, size_t *out_len);

/* Tool-result limits applied before output is sent to the model. */
size_t output_cap_bytes(void);
#define OUTPUT_CAP_LINES      2000
#define OUTPUT_CAP_LINE_WIDTH 500

/* Visible separator between read-tool line numbers and content. Unlike whitespace, it cannot be
 * mistaken for indentation when a model copies text into an edit request. */
#define READ_LINE_DELIM "\xE2\x86\x92"

/* Append-only byte buffer, NUL-terminated whenever data is non-NULL. */
struct buf {
    char *data;
    size_t len;
    size_t cap;
};

void buf_init(struct buf *buf);
void buf_free(struct buf *buf);
void buf_append(struct buf *buf, const void *data, size_t length);
void buf_append_str(struct buf *buf, const char *str);
void buf_reset(struct buf *buf);
/* Transfer an allocated NUL-terminated string to the caller and reset buf. */
char *buf_steal(struct buf *buf);

#endif /* HAX_UTIL_H */
