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

/* Join two filesystem path components with exactly one '/' between them.
 * Strips trailing slashes from `base` (preserving "/" itself as root)
 * and leading slashes from `rel`. Use for path concatenation where
 * either side may have come from an env var (TMPDIR on macOS ends in
 * "/") or a path-walk that lands on root. Both args must be non-NULL;
 * callers passing user/env data should fall back to a literal default
 * before calling. Caller frees. */
char *path_join(const char *base, const char *rel);

/* Shared limits applied to tool results (bash, read) before they go back
 * to the model. Three knobs:
 *
 *   - byte cap: the only one that scales with the host model's context
 *     size. Env-tunable via HAX_TOOL_OUTPUT_CAP using parse_size grammar
 *     ("25k", "200k", "1m"). 50 KiB default — matches opencode/pi-mono
 *     and is roughly 12K tokens, a sensible bite for both small local
 *     models and frontier ones.
 *
 *   - line count cap: guardrail against "10000 short lines" output
 *     shapes that don't trip the byte cap but still drown context. Not
 *     env-tunable — the right value doesn't change with model size.
 *
 *   - per-line width cap: guardrail against single-line megabyte
 *     pathologies (minified JS, log lines without newlines). Same
 *     reasoning: not model-dependent, hardcoded.
 *
 * Whichever cap fires first wins. */
size_t output_cap_bytes(void);

#define OUTPUT_CAP_LINES      2000
#define OUTPUT_CAP_LINE_WIDTH 500

/* Parse a size with optional k/m suffix (case-insensitive, 1024-base):
 *   "256k" → 262144, "128K" → 131072, "1m" → 1048576, "4096" → 4096.
 * Returns 0 on empty/invalid input — callers using 0 as a "disabled"
 * sentinel can't distinguish, but every current caller wants a positive
 * size and falls back to a hardcoded default on 0. */
long parse_size(const char *s);

/* Parse a duration with optional ms/s/m/h suffix (case-insensitive):
 *   "30" → 30000 (no suffix = seconds, the common case)
 *   "30s" → 30000, "30ms" → 30, "5m" → 300000, "2h" → 7200000.
 * Whitespace between the number and suffix is allowed. Returns the
 * duration in milliseconds, or -1 on empty/invalid input (so 0 remains
 * a valid "disabled" value for callers that treat it as a sentinel). */
long parse_duration_ms(const char *s);

/* CLOCK_MONOTONIC milliseconds since an unspecified epoch — used for
 * elapsed-time math (timeouts, idle thresholds, animation cadence). */
long monotonic_ms(void);

/* Write a random UUIDv4 (36 chars + NUL terminator) to out. Aborts on
 * failure (e.g. /dev/urandom unavailable) — same convention as xmalloc. */
void gen_uuid_v4(char out[37]);

/* Host terminal width via TIOCGWINSZ on stdout. Falls back to 120 cols
 * when stdout isn't a TTY or the ioctl fails, and clamps the result to
 * [40, 200] so callers don't need to defend against pathologically
 * narrow or wide widths. Re-queried on each call so SIGWINCH-style
 * resizes are picked up without explicit signal handling.
 *
 * Use term_width() only when you need the *real* edge of the row —
 * cursor positioning, ANSI erase-line, spinner placement. For content
 * layout (header reflow, tool previews, future markdown wrapping)
 * prefer display_width() which caps the result for readability. */
int term_width(void);

/* Soft cap on content width applied by display_width(). Lines wrapped
 * to the display width stay at a readable length even on ultrawide
 * terminals (web-style "max-width" idiom). Tweak here if it ever
 * needs to grow. */
#define DISPLAY_WIDTH_CAP 100

/* Capped variant of term_width() for content layout — clamps the
 * result to DISPLAY_WIDTH_CAP cells. Use anywhere width drives word
 * wrapping or text truncation; the unclamped term_width() stays for
 * cursor-edge concerns where the real terminal column matters. */
int display_width(void);

/* Truncate a UTF-8 string to fit in `cap` visual cells, replacing the
 * cut suffix with "..." so the user sees an explicit "more here"
 * marker. Returns a fresh dup when content already fits. Width is
 * via utf8_codepoint_cells — locale-dependent, requires
 * locale_init_utf8() at startup. Returns malloc'd; caller frees. */
char *truncate_for_display(const char *s, size_t cap);

/* Find the byte offset where to break `s` (length `len`) so the
 * current row fits in at most `max_cells` visual cells. Returns the
 * end-of-row byte offset (s[0..return) is the row content). When
 * *resume_at is non-NULL, also reports the byte offset where the
 * next row's content starts — differs from the end offset when the
 * break consumes a separating space.
 *
 * Algorithm: walks forward codepoint-by-codepoint, accumulating cell
 * widths via utf8_codepoint_cells. The rightmost ASCII space within
 * the budget is the break point (end excludes the space, resume
 * skips it). A space sitting exactly at column max_cells is also a
 * valid break — its width belongs to the inter-row fence. If the row
 * holds no space at all, hard-breaks at the codepoint boundary that
 * would push past max_cells. When the whole input fits, returns len.
 *
 * Width is measured in cells — see truncate_for_display for the
 * locale caveat.
 *
 * Precondition: max_cells >= 1. A zero-width row has no meaningful
 * break position (would stall the caller's loop) — callers must
 * clamp first. reflow_for_display clamps internally; the streaming-
 * markdown wrapper will need to do the same.
 *
 * Stateless primitive; both reflow_for_display and the (upcoming)
 * streaming markdown wrapper layer their own state on top. */
size_t wrap_break_pos(const char *s, size_t len, size_t max_cells, size_t *resume_at);

/* Reflow `s` so it fits in at most `max_rows` terminal rows, breaking
 * at word boundaries (ASCII spaces). Long unbroken words hard-break
 * at the row boundary. If content exceeds the budget, the last
 * visible row is truncated with a trailing "..." marker.
 *
 *   first_row         — cells available on the first row (caller may
 *                       have a prefix already laid down, e.g.
 *                       "[bash] ")
 *   mid_row           — cells on subsequent rows (full row width)
 *   max_rows          — maximum rows of output (>= 1)
 *   last_row_reserve  — cells to reserve at the end of the last row
 *                       for a suffix the caller will append after this
 *                       (e.g. read's ":N-M" extra). 0 if none.
 *
 * Returns malloc'd; rows are joined by '\n' (no trailing newline).
 * The returned string never contains ANSI escapes — caller wraps with
 * styling. NULL input returns "". Caller frees.
 *
 * Width is measured in cells — see truncate_for_display for the
 * locale caveat. */
char *reflow_for_display(const char *s, int first_row, int mid_row, int max_rows,
                         int last_row_reserve);

/* Prepare an arbitrary UTF-8 string for one-line display. Three
 * passes in one walk:
 *
 *   - Replace ASCII control bytes (newline, CR, tab, etc.) with
 *     single spaces and collapse runs of whitespace to one space;
 *     strip leading/trailing whitespace. Lets multi-line content
 *     (a bash command, a JSON arg) render on a single visual line.
 *
 *   - Substitute one '?' per "dangerous" multi-byte codepoint —
 *     Trojan Source bidi vectors, ZWJ and other invisibles, malformed
 *     UTF-8. Without this, a model-supplied path or shell command
 *     could embed bidi overrides and have the rendered header
 *     reordered or hidden in the terminal. Mirrors the cell-width
 *     substitution policy of utf8_codepoint_cells.
 *
 *   - Cap consecutive zero-width codepoints (combining marks, VS-N)
 *     at a small bound per base glyph. Legitimate scripts use 0-6;
 *     the cap stops adversarial floods of marks that nominally take
 *     ~1 cell but consume arbitrary bytes.
 *
 *   - Pass through everything else (printable ASCII, well-formed
 *     multi-byte codepoints with non-negative wcwidth) verbatim.
 *
 * Locale-dependent — the dangerous-codepoint detection routes through
 * mbrtowc + wcwidth, so locale_init_utf8() must run at startup.
 *
 * Returns malloc'd; caller frees. NULL input returns "". */
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
