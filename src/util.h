/* SPDX-License-Identifier: MIT */
#ifndef HAX_UTIL_H
#define HAX_UTIL_H

#include <stddef.h>

/* Set LC_CTYPE — and only LC_CTYPE — to a UTF-8 locale so libedit's
 * wide-char machinery can handle multibyte input and prompt bytes. We
 * deliberately avoid LC_ALL/"" so the user's LC_NUMERIC etc. don't slip
 * in and break printf/JSON output (e.g. German "1,5" instead of "1.5").
 * Cascade: env-defined LC_CTYPE → C.UTF-8 → en_US.UTF-8. Idempotent and
 * safe to call before anything else; should be the first thing in main. */
void locale_init_utf8(void);

/* True iff locale_init_utf8() established a UTF-8 LC_CTYPE. Callers that
 * emit multibyte content through wide-aware libraries (libedit) should
 * fall back to ASCII when this returns 0. */
int locale_have_utf8(void);

/* Allocate-or-die helpers. On OOM they print to stderr and abort — we are a
 * CLI, not a library; a clean crash is better than leaking partial state. */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Reject anything that isn't a regular file before opening it. open()
 * on a FIFO without a writer blocks indefinitely, which would freeze a
 * startup that touches a path the user doesn't fully control (config
 * files, the history file, tool inputs). Returns 0 if `path` exists
 * and is a regular file, -1 otherwise with errno set (EISDIR or EINVAL
 * when stat succeeded but the type was wrong). There's a tiny TOCTOU
 * window between stat and the subsequent open; the alternative (open
 * with O_NONBLOCK then fstat) doesn't reliably help because the FIFO
 * open is what blocks, before fstat runs. */
int ensure_regular_file(const char *path);

/* Read an entire file into a newly-allocated NUL-terminated string.
 * Returns NULL on error and sets errno. Caller frees. Rejects anything
 * that isn't a regular file (errno EISDIR for directories, EINVAL for
 * FIFOs / sockets / devices) so an unlucky path can't block startup. */
char *slurp_file(const char *path, size_t *out_len);

/* Write exactly n bytes to fd, restarting on EINTR/short writes. Returns 0
 * on success, -1 on error (errno set). */
int write_all(int fd, const void *data, size_t n);

/* Read up to cap bytes from path into a newly-allocated NUL-terminated
 * string. Allocates at most cap+1 bytes regardless of file size. If the
 * file has more bytes than the cap, sets *out_truncated to 1. Returns
 * NULL on error and sets errno. Caller frees. Same regular-file guard
 * as slurp_file. */
char *slurp_file_capped(const char *path, size_t cap, size_t *out_len, int *out_truncated);

/* Expand a leading ~ to $HOME. Returns a newly-allocated path. */
char *expand_home(const char *path);

/* Resolve hax's XDG-style paths under the per-user config and state
 * trees, both namespaced as `<base>/hax/<relpath>`:
 *   xdg_hax_config_path("AGENTS.md") -> $XDG_CONFIG_HOME/hax/AGENTS.md,
 *     or $HOME/.config/hax/AGENTS.md as fallback.
 *   xdg_hax_state_path("history") -> $XDG_STATE_HOME/hax/history, or
 *     $HOME/.local/state/hax/history as fallback.
 * Config holds user-edited preferences (AGENTS.md, skills); state
 * holds runtime/volatile data (history). Returns NULL when neither
 * the env var nor $HOME is set. Caller frees. */
char *xdg_hax_config_path(const char *relpath);
char *xdg_hax_state_path(const char *relpath);

/* Duplicate `s` with any trailing '/' characters stripped. Lets callers
 * normalize a base URL so "http://x/v1/" and "http://x/v1" produce the
 * same downstream concatenation. Caller frees. */
char *dup_trim_trailing_slash(const char *s);

/* Parse a duration with optional ms/s/m/h suffix (case-insensitive):
 *   "30" → 30000 (no suffix = seconds, the common case)
 *   "30s" → 30000, "30ms" → 30, "5m" → 300000, "2h" → 7200000.
 * Whitespace between the number and suffix is allowed. Returns the
 * duration in milliseconds, or -1 on empty/invalid input (so 0 remains
 * a valid "disabled" value for callers that treat it as a sentinel). */
long parse_duration_ms(const char *s);

/* Write a random UUIDv4 (36 chars + NUL terminator) to out. Aborts on
 * failure (e.g. /dev/urandom unavailable) — same convention as xmalloc. */
void gen_uuid_v4(char out[37]);

/* Host terminal width via TIOCGWINSZ on stdout. Falls back to 120 cols
 * when stdout isn't a TTY or the ioctl fails, and clamps the result to
 * [40, 200] so callers don't need to defend against pathologically
 * narrow or wide widths. Re-queried on each call so SIGWINCH-style
 * resizes are picked up without explicit signal handling. */
int term_width(void);

/* Replace ASCII control bytes (newline, CR, tab, etc.) with single spaces
 * and collapse runs of whitespace to one space, stripping leading and
 * trailing whitespace. Used to render multi-line content (a bash command
 * like `ls\npwd`, a JSON arg) on a single visual line for headers and
 * logs. Returns a newly-allocated NUL-terminated string; caller frees. */
char *flatten_for_display(const char *s);

/* Truncate any line in `data` longer than `max_line` bytes to its first
 * `max_line` bytes followed by an inline `...[N bytes elided]` marker.
 * Newline structure is preserved (one input line → one output line) so
 * line-counting downstream still works. Returns a newly-allocated
 * NUL-terminated string with the result; *out_len receives its byte
 * length. Caller frees. When no line exceeds the cap, the returned
 * buffer is a freshly-allocated copy of the input (so the caller can
 * unconditionally free both old and new pointers). */
char *cap_line_lengths(const char *data, size_t len, size_t max_line, size_t *out_len);

/* Dynamic byte buffer — append-only, NUL-terminated at current length. */
struct buf {
    char *data;
    size_t len;
    size_t cap;
};

void buf_init(struct buf *b);
void buf_free(struct buf *b);
void buf_append(struct buf *b, const void *data, size_t n);
void buf_append_str(struct buf *b, const char *s);
void buf_reset(struct buf *b);
char *buf_steal(struct buf *b); /* takes ownership, re-inits buf */

#endif /* HAX_UTIL_H */
