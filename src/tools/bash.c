/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "config.h"
#include "tool.h"
#include "util.h"
#include "system/path.h"
#include "terminal/interrupt.h"
#include "text/utf8.h"
#include "text/utf8_sanitize.h"
#include "tools/bash_cd_strip.h"
#include "tools/bash_classify.h"
#include "tools/bash_export.h"
#include "tools/bash_shell.h"

/* Output is captured to a temp file (mkstemp under $TMPDIR) so the model
 * sees a tail-truncated preview but can `read` the full output afterwards
 * via the path embedded in the truncation hint. The temp file is unlinked
 * when the output fits within the OUTPUT_CAP_* limits; otherwise it's
 * kept for the model to revisit. We deliberately don't GC kept files at
 * end of session — the model may reference one many turns later, and the
 * OS evicts /tmp on reboot. MAX_DRAIN_BYTES caps how much the reader is
 * willing to capture from a runaway producer (`yes`, `cat /dev/urandom`) —
 * the file size never exceeds this, and the producer is SIGKILLed once
 * we hit it. The effective byte cap (BASH_BYTE_CAP_MAX) is clamped
 * strictly below MAX_DRAIN_BYTES so spill always fires before drain — a
 * user-supplied HAX_TOOL_OUTPUT_CAP=20m must not let mem grow past the
 * drain SIGKILL with no truncation marker. */
#define MAX_DRAIN_BYTES   (16L * 1024 * 1024)
#define BASH_BYTE_CAP_MAX (MAX_DRAIN_BYTES - 64L * 1024)

/* Saturating add for non-negative longs. A configured duration near
 * LONG_MAX (e.g. HAX_BASH_TIMEOUT="9223372036854775000ms" or a per-call
 * arg with the max ceiling disabled) would otherwise overflow `now +
 * delta` — UB, and on typical wraparound the result is a large negative
 * deadline that fires the timeout on iteration zero. */
static long sat_add(long now, long delta)
{
    return delta > LONG_MAX - now ? LONG_MAX : now + delta;
}

/* Send `sig` to the child's process group, falling back to the bare
 * pid if the group doesn't yet exist. The child runs setsid() after
 * fork() to become a session leader (and thus its own pgroup leader
 * with pgid == pid), but that happens *after* fork() returns to the
 * parent — there's a small window where the parent can reach
 * kill(-pid, …) before the child has finished session setup. In that
 * window the pgroup pid doesn't exist and kill(-pid, …) fails with
 * ESRCH, which with HAX_BASH_TIMEOUT_GRACE=0 would let the loop break
 * and block in waitpid() until the command exits naturally. The
 * fallback to kill(pid, …) is safe in that window: the child hasn't
 * exec'd the shell yet, so there are no descendants to leak.
 *
 * Caller invariant: pid must NOT have been reaped before we get here.
 * After the kernel reaps the pid, it can recycle it on the next
 * fork() and a kill(pid, …) fallback could target an unrelated
 * process. run_shell upholds this by using waitid(WNOWAIT) in its
 * in-loop status check (peek without reap), keeping the zombie around
 * — and thus the pid allocated — until the post-loop blocking
 * waitpid does the actual reap. */
static void kill_descendants(pid_t pid, int sig)
{
    if (kill(-pid, sig) < 0 && errno == ESRCH)
        kill(pid, sig);
}

/* Render the configured timeout for the model. Whole seconds use "Ns"
 * for readability; sub-second durations stay in "Nms" so they aren't
 * silently rounded to "0s". */
static void format_timeout_for_model(char *buf, size_t buflen, long timeout_ms)
{
    if (timeout_ms % 1000 == 0)
        snprintf(buf, buflen, "%lds", timeout_ms / 1000);
    else
        snprintf(buf, buflen, "%ldms", timeout_ms);
}

/* Hybrid byte capture: in-memory buffer up to the OUTPUT_CAP_* limits,
 * then spill to a temp file. Small commands never touch disk; long-
 * running ones (build logs, find, large diffs) accumulate in /tmp/hax-
 * bash-XXXXXX and are preserved past truncation so the model can `read`
 * them.
 *
 * Spill triggers on either byte cap or line cap — counting newlines per
 * chunk is cheap and lets shapes like "100k short lines" preserve early
 * lines through the file rather than silently dropping them.
 *
 * Once spilled, mem is freed and the file holds the canonical content;
 * we read its tail at finalization. Path stays NULL until spill, so
 * build_body_and_trunc uses it to decide preserve-vs-unlink behavior. */
struct capture {
    struct buf mem;     /* pre-spill bytes (full output if !spilled) */
    int spilled;        /* set once mem was flushed to file */
    int fd;             /* temp file fd, -1 until spilled */
    char *path;         /* malloc'd temp file path, NULL until spilled */
    int write_failed;   /* sticky: a write_all on the temp file errored */
    size_t total_bytes; /* running count, equals file size when spilled */
    size_t total_lines; /* newline count across the full output */
    int trails_no_nl;   /* the last byte written wasn't '\n' */
};

static void capture_init(struct capture *c)
{
    buf_init(&c->mem);
    c->spilled = 0;
    c->fd = -1;
    c->path = NULL;
    c->write_failed = 0;
    c->total_bytes = 0;
    c->total_lines = 0;
    c->trails_no_nl = 0;
}

static size_t count_newlines(const char *data, size_t n)
{
    size_t k = 0;
    const char *p = data;
    size_t left = n;
    while (left > 0) {
        const char *nl = memchr(p, '\n', left);
        if (!nl)
            break;
        k++;
        size_t consumed = (size_t)(nl - p) + 1;
        p = nl + 1;
        left -= consumed;
    }
    return k;
}

/* Per-session registry of preserved temp files. capture_open_tempfile
 * records each one; capture_unlink drops the entry. What's left is what
 * we kept past truncation — bash_cleanup_tempfiles unlinks all of them
 * on /new (via agent_new_conversation) and on process exit. Without
 * this, the model-facing path embedded in the truncation marker leaks
 * a file that nothing else cleans up: macOS doesn't evict /var/folders
 * for days, and Linux /tmp may live until reboot. Single-threaded
 * (tools run on the agent loop), so no locking.
 *
 * These files don't outlive the process, so after a session is resumed
 * the persisted "full output saved to <path>" marker points at a path
 * that's gone. We deliberately leave the marker verbatim rather than
 * rewrite it on load: editing history would break prompt-cache reuse on a
 * soon-after resume (the common case), the truncated head/tail is still
 * inline, and a read of the missing path just yields a recoverable error
 * the model re-runs past. (Surviving resume would mean session-scoped
 * spills — see Claude Code — at the cost of unbounded growth in a tool
 * with no "session done" hook; not worth it here.) */
static char **kept_paths;
static size_t kept_paths_n;
static size_t kept_paths_cap;
static int cleanup_atexit_registered;

void bash_cleanup_tempfiles(void)
{
    for (size_t i = 0; i < kept_paths_n; i++) {
        if (kept_paths[i]) {
            unlink(kept_paths[i]);
            free(kept_paths[i]);
        }
    }
    kept_paths_n = 0;
}

static void track_kept_path(const char *path)
{
    if (!cleanup_atexit_registered) {
        atexit(bash_cleanup_tempfiles);
        cleanup_atexit_registered = 1;
    }
    if (kept_paths_n == kept_paths_cap) {
        kept_paths_cap = kept_paths_cap ? kept_paths_cap * 2 : 4;
        kept_paths = xrealloc(kept_paths, kept_paths_cap * sizeof(*kept_paths));
    }
    kept_paths[kept_paths_n++] = xstrdup(path);
}

static void untrack_kept_path(const char *path)
{
    for (size_t i = 0; i < kept_paths_n; i++) {
        if (kept_paths[i] && strcmp(kept_paths[i], path) == 0) {
            free(kept_paths[i]);
            /* Order doesn't matter for cleanup, so swap-with-last is the
             * cheapest removal. */
            kept_paths_n--;
            kept_paths[i] = kept_paths[kept_paths_n];
            kept_paths[kept_paths_n] = NULL;
            return;
        }
    }
}

static int is_valid_utf8(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        int sl = utf8_seq_len((unsigned char)s[i]);
        if (sl <= 0 || i + (size_t)sl > len || !utf8_seq_valid(s + i, sl))
            return 0;
        i += (size_t)sl;
    }
    return 1;
}

/* Open /tmp/hax-bash-XXXXXX exclusively (mkstemp gives us O_RDWR, mode
 * 0600). On failure we set write_failed so the rest of the run becomes
 * a no-op for capture — the command still runs to completion and the
 * model gets the body that fit in mem before the spill attempt.
 *
 * Falls back to "/tmp" when $TMPDIR contains bytes that aren't valid
 * UTF-8: the path travels back to the model embedded in the truncation
 * marker, so it must round-trip cleanly through the provider's JSON
 * encoder (jansson). Without the fallback we'd advertise a sanitized
 * path that doesn't exist on disk — the actual file lives at the raw-
 * byte path, but the model can only read what the marker says. */
static int capture_open_tempfile(struct capture *c)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp || !is_valid_utf8(tmp, strlen(tmp)))
        tmp = "/tmp";
    c->path = path_join(tmp, "hax-bash-XXXXXX");
    c->fd = mkstemp(c->path);
    if (c->fd < 0) {
        free(c->path);
        c->path = NULL;
        c->write_failed = 1;
        return -1;
    }
    track_kept_path(c->path);
    return 0;
}

static void capture_spill(struct capture *c)
{
    /* Set spilled=1 unconditionally — even on open failure — so subsequent
     * capture_write calls take the "already spilled" branch and don't
     * retry mkstemp on every chunk for the rest of the run.
     *
     * On open failure we deliberately keep c->mem alive: it holds the
     * pre-overflow prefix, which is the best body we can offer the model
     * when the temp file isn't available. capture_write's spilled-and-
     * write_failed branch then drops further bytes (no place to put
     * them), and build_body_and_trunc surfaces the prefix as a truncated
     * result with a "(temp file unavailable)" marker. Without this
     * fallback we'd silently lose a usable prefix on transient errors
     * like a misconfigured TMPDIR. */
    c->spilled = 1;
    if (capture_open_tempfile(c) < 0)
        return;
    if (c->mem.len > 0 && write_all(c->fd, c->mem.data, c->mem.len) < 0)
        c->write_failed = 1;
    buf_free(&c->mem);
}

/* Append a chunk to the capture, spilling to disk first if either cap
 * would be exceeded. Spill is one-way; once on disk we stay on disk for
 * the rest of the command. Errors are sticky on write_failed but never
 * abort the run — the command keeps going so the model still sees an
 * exit code. total_bytes / total_lines update unconditionally (even past
 * write_failed or beyond MAX_DRAIN_BYTES) because the marker reports
 * what the producer attempted, not what we managed to capture. */
static void capture_write(struct capture *c, const char *data, size_t n, size_t cap_bytes)
{
    if (n == 0)
        return;
    size_t new_lines = count_newlines(data, n);
    c->total_bytes += n;
    c->total_lines += new_lines;
    c->trails_no_nl = (data[n - 1] != '\n');
    if (c->write_failed && c->spilled)
        return;
    if (!c->spilled) {
        /* Compare against the human-visible line count (newlines plus the
         * trailing partial line, if any). Using c->total_lines alone would
         * miss a "2000 newlines + 1 unterminated line" output, where the
         * partial 2001st line keeps total_lines pegged at 2000. */
        size_t effective_lines = c->total_lines + (c->trails_no_nl ? 1 : 0);
        if (c->mem.len + n <= cap_bytes && effective_lines <= OUTPUT_CAP_LINES) {
            buf_append(&c->mem, data, n);
            return;
        }
        capture_spill(c);
        if (c->write_failed)
            return;
    }
    if (write_all(c->fd, data, n) < 0)
        c->write_failed = 1;
}

static void capture_free(struct capture *c)
{
    buf_free(&c->mem);
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    free(c->path);
    c->path = NULL;
}

/* Drop the temp file (used when the captured output fits within caps and
 * the path won't be referenced from the result). Safe to call even when
 * !spilled — it's a no-op then. */
static void capture_unlink(struct capture *c)
{
    if (c->path) {
        unlink(c->path);
        untrack_kept_path(c->path);
        free(c->path);
        c->path = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

/* Format a byte count in 1024-base: "412B", "5.4K", "50K", "1.2M". Used
 * inside the truncation hint where the model benefits from a compact
 * size relative to the cap. */
static void format_byte_size(char *buf, size_t bufsize, size_t bytes)
{
    if (bytes < 1024)
        snprintf(buf, bufsize, "%zuB", bytes);
    else if (bytes < 10L * 1024)
        snprintf(buf, bufsize, "%.1fK", (double)bytes / 1024.0);
    else if (bytes < 1024L * 1024)
        snprintf(buf, bufsize, "%zuK", (bytes + 512) / 1024);
    else if (bytes < 10L * 1024 * 1024)
        snprintf(buf, bufsize, "%.1fM", (double)bytes / (1024.0 * 1024.0));
    else
        snprintf(buf, bufsize, "%zuM", (bytes + 512L * 1024) / (1024L * 1024));
}

/* The truncation budget (output_cap_bytes() / OUTPUT_CAP_LINES) is split
 * between a head slice and a tail slice so that output whose summary sits
 * at the top — `git show --stat`, `ls`, `git log`, build banners — keeps
 * that summary instead of losing it to tail-only truncation, while the
 * trailing payload (errors, results) still survives. The split is
 * asymmetric: heads are short summaries, tails carry the meat, so the head
 * gets 1/HEAD_DIVISOR of each budget and the tail the rest. The head+tail
 * total stays within the original caps, so context cost is unchanged. */
#define OUTPUT_HEAD_DIVISOR 8

/* cap_line_lengths + sanitize_utf8 a raw segment and append it to `body`.
 * cap_line_lengths cuts over-long lines at a byte boundary that can split a
 * UTF-8 codepoint; sanitize_utf8 then repairs the orphaned bytes so the
 * result is always valid UTF-8 (jansson rejects invalid UTF-8 in
 * json_string). Shared by the head, tail, and whole-output paths. */
static void append_clean(struct buf *body, const char *data, size_t len)
{
    size_t capped_len = 0;
    char *capped = cap_line_lengths(data ? data : "", len, OUTPUT_CAP_LINE_WIDTH, &capped_len);
    char *clean = sanitize_utf8(capped, capped_len);
    free(capped);
    buf_append_str(body, clean);
    free(clean);
}

/* Read the first `cap_bytes` of the spilled file (but never past
 * `limit_off`, where the tail window begins, so the head and tail windows
 * can never overlap), drop any trailing partial line so the head ends on a
 * line boundary, then keep at most `cap_lines` leading lines. If the window
 * holds no newline at all (one line longer than the head budget) we keep
 * nothing and let the caller fall back to tail-only — splitting a single
 * logical line across the gap would only confuse the model. Reports kept
 * bytes/lines via out params. */
static int read_head_capped(int fd, size_t cap_bytes, size_t cap_lines, off_t limit_off,
                            struct buf *out, size_t *kept_bytes_out, size_t *kept_lines_out)
{
    *kept_bytes_out = 0;
    *kept_lines_out = 0;
    size_t want = cap_bytes;
    if (limit_off >= 0 && (off_t)want > limit_off)
        want = (size_t)limit_off;
    if (want == 0)
        return 0;
    if (lseek(fd, 0, SEEK_SET) < 0)
        return -1;

    char chunk[8192];
    while (out->len < want) {
        size_t need = want - out->len;
        ssize_t r = read(fd, chunk, need < sizeof(chunk) ? need : sizeof(chunk));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        buf_append(out, chunk, (size_t)r);
    }

    /* Trim the trailing partial line: keep through the last '\n'. With no
     * newline the whole window is one unterminated line — keep nothing. */
    size_t last_nl = out->len;
    while (last_nl > 0 && out->data[last_nl - 1] != '\n')
        last_nl--;
    if (last_nl == 0) {
        out->len = 0;
        if (out->data)
            out->data[0] = '\0';
        return 0;
    }
    out->len = last_nl;

    /* Keep at most cap_lines leading lines (each ends in '\n' now). */
    size_t lines = count_newlines(out->data, out->len);
    if (lines > cap_lines) {
        size_t seen = 0, i = 0;
        while (i < out->len && seen < cap_lines) {
            if (out->data[i] == '\n')
                seen++;
            i++;
        }
        out->len = i;
        lines = cap_lines;
    }
    if (out->data)
        out->data[out->len] = '\0';

    *kept_bytes_out = out->len;
    *kept_lines_out = lines;
    return 0;
}

/* Read the last `cap_bytes` of the spilled file into `out`, then align
 * the front to a line boundary so we never start mid-line — except when
 * the entire window contains no newline (a single-line output > cap),
 * in which case we keep the bytes raw and let cap_line_lengths emit the
 * inline elision marker downstream. Then trim the front so at most
 * `cap_lines` newline-terminated lines remain. Reports the kept-
 * byte and kept-line counts via out params (not derivable from `out->len`
 * alone because the caller wants pre-cap_line_lengths/pre-sanitize
 * numbers in the hint). */
static int read_tail_capped(int fd, size_t total_bytes, size_t cap_bytes, size_t cap_lines,
                            struct buf *out, size_t *kept_bytes_out, size_t *kept_lines_out)
{
    size_t want = total_bytes < cap_bytes ? total_bytes : cap_bytes;
    off_t start = (off_t)(total_bytes - want);

    /* Decide whether the tail window already starts on a line boundary
     * by peeking the byte immediately before `start`. If that byte is
     * '\n', the byte at `start` is the first byte of a fresh line and
     * we keep it verbatim — without this check, the alignment trim
     * below would drop a complete leading line whenever the cap
     * happened to land exactly on a line boundary. pread() doesn't
     * disturb the read offset, so we can issue it before the lseek. */
    int needs_alignment = 0;
    if (start > 0) {
        char prev;
        ssize_t pr;
        do {
            pr = pread(fd, &prev, 1, start - 1);
        } while (pr < 0 && errno == EINTR);
        /* On read failure, fall back to the conservative behavior of
         * trimming through the first '\n'. We'd rather lose one line
         * than start mid-line. */
        needs_alignment = (pr != 1) || (prev != '\n');
    }

    if (lseek(fd, start, SEEK_SET) < 0)
        return -1;

    char chunk[8192];
    for (;;) {
        ssize_t r = read(fd, chunk, sizeof(chunk));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        buf_append(out, chunk, (size_t)r);
    }

    /* Line-boundary alignment: when start>0 and the window starts
     * mid-line, drop everything up to and including the first '\n'.
     * If no '\n' exists or the only '\n' is at the very end (a single
     * line longer than cap_bytes that does end with '\n'), trimming
     * would leave an empty body — keep the raw tail bytes and let
     * downstream line-width capping emit an inline elision marker. */
    if (needs_alignment && out->len > 0) {
        char *nl = memchr(out->data, '\n', out->len);
        if (nl) {
            size_t skip = (size_t)(nl - out->data) + 1;
            if (skip < out->len) {
                memmove(out->data, out->data + skip, out->len - skip);
                out->len -= skip;
                if (out->data)
                    out->data[out->len] = '\0';
            }
            /* skip == out->len: the only '\n' is the last byte. Trimming
             * would empty the buffer, taking the over-long line's tail
             * with it. Leave the buffer alone. */
        }
    }

    /* Trim the front so at most `cap_lines` lines remain. Count
     * newlines in the buffer; if the count is over budget, advance past
     * the first (count - cap) newlines and keep the rest. */
    size_t total_lines = count_newlines(out->data ? out->data : "", out->len);
    /* A trailing line without a final newline counts too. */
    size_t lines_in_buf = total_lines + (out->len > 0 && out->data[out->len - 1] != '\n' ? 1 : 0);
    if (lines_in_buf > cap_lines) {
        size_t to_skip = lines_in_buf - cap_lines;
        size_t i = 0;
        while (to_skip > 0 && i < out->len) {
            if (out->data[i] == '\n')
                to_skip--;
            i++;
        }
        memmove(out->data, out->data + i, out->len - i);
        out->len -= i;
        if (out->data)
            out->data[out->len] = '\0';
        lines_in_buf = cap_lines;
    }

    *kept_bytes_out = out->len;
    *kept_lines_out = lines_in_buf;
    return 0;
}

/* Append the trailing exit/timeout/interrupt status to a result buffer.
 * Interrupt takes precedence over timeout — if both fire (rare: deadline
 * hits during the grace window of a user-Esc), the user-driven cause is
 * the more useful explanation. */
static void append_footers(struct buf *out, int timed_out, int interrupted, long timeout_ms,
                           int status)
{
    if (interrupted) {
        buf_append_str(out, "\n[interrupted]");
    } else if (timed_out) {
        char human[32];
        format_timeout_for_model(human, sizeof(human), timeout_ms);
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "\n[timed out after %s]", human);
        buf_append_str(out, tmp);
    } else if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code != 0) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "\n[exit %d]", code);
            buf_append_str(out, tmp);
        }
    } else if (WIFSIGNALED(status)) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "\n[signal %d]", WTERMSIG(status));
        buf_append_str(out, tmp);
    }
}

/* Append the trailing portion that follows whatever body has already
 * been written to `out`:
 *   - binary marker (when has_nul)
 *   - exit/timeout/interrupt footer
 *   - "(no output)" fallback when nothing else was appended in this
 *     call AND no body was produced (body_present=0)
 *
 * The truncation marker is not emitted here. The model gets it embedded in
 * its body by build_body_and_trunc (a gap marker between the head and tail
 * slices, or an end marker in the tail-only fallback). The live display
 * conveys truncation through the renderer's own head/tail elision marker
 * ("... (N more lines) ..."), so a second marker here would be redundant
 * and would report a different (display-row vs model-line) count. That
 * leaves this suffix byte-identical for the canonical-history and
 * live-display paths. `total_bytes` is only consulted for the binary
 * marker. */
static void append_run_suffix(struct buf *out, size_t total_bytes, int has_nul, int body_present,
                              int timed_out, int interrupted, long timeout_ms, int status)
{
    size_t before = out->len;
    if (has_nul) {
        char total_b[16];
        format_byte_size(total_b, sizeof(total_b), total_bytes);
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "[binary output suppressed: %s]", total_b);
        buf_append_str(out, tmp);
    }
    append_footers(out, timed_out, interrupted, timeout_ms, status);
    if (out->len == before && !body_present)
        buf_append_str(out, "(no output)");
}

/* Append a model-facing truncation marker (owned `marker`) to the body,
 * sanitized: the saved-to path comes from $TMPDIR + mkstemp and POSIX paths
 * carry no UTF-8 guarantee, so the raw bytes would make jansson reject the
 * next request (dropping the result or breaking the conversation). */
static void append_clean_marker(struct buf *body, char *marker)
{
    char *clean = sanitize_utf8(marker, strlen(marker));
    free(marker);
    buf_append_str(body, clean);
    free(clean);
}

/* Build the model-facing body: sanitized, line-width-capped, with the
 * truncation marker embedded when truncation happened — a gap marker
 * between a head and a tail slice, or an end marker in the tail-only
 * fallback. The live display does NOT use this marker; it relies on the
 * renderer's own head/tail elision. Unlinks the temp file when it isn't
 * needed. The caller still owns `cap`; this only reads from it (and may
 * unlink the spilled file). */
static void build_body_and_trunc(struct capture *cap, int has_nul, struct buf *body)
{
    if (has_nul) {
        capture_unlink(cap);
        return;
    }

    if (!cap->spilled) {
        /* Whole output fit in mem; no truncation. */
        append_clean(body, cap->mem.data, cap->mem.len);
        capture_unlink(cap);
        return;
    }

    /* count_newlines only counts '\n' terminators; if the producer's final
     * line was unterminated, add it so the marker's "of N lines" reflects
     * the human-visible line count. */
    size_t total_lines = cap->total_lines + (cap->trails_no_nl ? 1 : 0);

    if (cap->fd < 0 || cap->write_failed) {
        /* Spill attempted but the temp file isn't usable (mkstemp failed or
         * write_all failed mid-flush). capture_spill kept c->mem alive in
         * the mkstemp-failure case so we can still serve the pre-spill
         * prefix; write_all-failure leaves mem.len==0 (partial file content
         * is unsafe to recover). Tail-only end marker, no path — the model
         * can re-run with grep/head/tail if it needs more. */
        size_t kept_bytes = 0, kept_lines = 0;
        if (cap->mem.len > 0) {
            append_clean(body, cap->mem.data, cap->mem.len);
            kept_bytes = cap->mem.len;
            kept_lines = count_newlines(cap->mem.data, cap->mem.len) +
                         (cap->mem.data[cap->mem.len - 1] != '\n' ? 1 : 0);
        }
        char kept_b[16], total_b[16];
        format_byte_size(kept_b, sizeof(kept_b), kept_bytes);
        format_byte_size(total_b, sizeof(total_b), cap->total_bytes);
        append_clean_marker(body, xasprintf("\n[output truncated: last %zu of %zu lines, %s of %s; "
                                            "full output unavailable (temp file write failed)]",
                                            kept_lines, total_lines, kept_b, total_b));
        capture_unlink(cap);
        return;
    }

    /* Spill succeeded. Split the budget into a tail (the bulk) and a small
     * head, read each line-aligned and line-count-capped, then assemble
     * head + gap marker + tail so output that front-loads its summary keeps
     * it. The head window is bounded below the tail's start offset so the
     * two can never overlap. */
    size_t total_cap = output_cap_bytes();
    size_t head_cap_bytes = total_cap / OUTPUT_HEAD_DIVISOR;
    size_t head_cap_lines = OUTPUT_CAP_LINES / OUTPUT_HEAD_DIVISOR;
    size_t tail_cap_bytes = total_cap - head_cap_bytes;
    size_t tail_cap_lines = OUTPUT_CAP_LINES - head_cap_lines;

    struct buf tail_raw;
    buf_init(&tail_raw);
    size_t tail_b = 0, tail_l = 0;
    if (read_tail_capped(cap->fd, cap->total_bytes, tail_cap_bytes, tail_cap_lines, &tail_raw,
                         &tail_b, &tail_l) != 0) {
        /* Spill read failed — no usable body. Surface a marker with no path
         * so the model knows output existed but can't be recovered. */
        buf_free(&tail_raw);
        append_clean_marker(body, xasprintf("\n[output truncated: %zu lines, full output "
                                            "unavailable (spill read failed)]",
                                            total_lines));
        capture_unlink(cap);
        return;
    }

    off_t tail_start_off = (off_t)(cap->total_bytes - tail_b);
    struct buf head_raw;
    buf_init(&head_raw);
    size_t head_b = 0, head_l = 0;
    /* A read_head_capped failure (lseek/read error) leaves head_b==0, which
     * falls through to the tail-only branch below — no special-casing. */
    read_head_capped(cap->fd, head_cap_bytes, head_cap_lines, tail_start_off, &head_raw, &head_b,
                     &head_l);

    /* Split into head + tail when the head holds at least one whole line
     * AND real content was dropped between the windows. Eligibility keys on
     * the byte gap, not the omitted-line count: a single long line can be
     * split across the windows (the tail window holds no newline, so it
     * starts mid-line) with a large byte gap but zero whole lines omitted —
     * gating on lines would drop the front-loaded summary this is meant to
     * keep. head_b <= tail_start_off (the head is bounded below the tail's
     * start), so the subtraction can't underflow. */
    size_t omitted_lines = total_lines > head_l + tail_l ? total_lines - head_l - tail_l : 0;
    size_t gap_bytes = (size_t)tail_start_off - head_b;
    if (head_l > 0 && gap_bytes > 0) {
        append_clean(body, head_raw.data, head_raw.len);
        char *marker;
        if (omitted_lines > 0) {
            marker = xasprintf("... [output truncated: omitted %zu of %zu lines (kept first %zu, "
                               "last %zu); full output saved to %s] ...\n",
                               omitted_lines, total_lines, head_l, tail_l, cap->path);
        } else {
            /* The gap falls within a single long line, so a line count would
             * read as a misleading "0 lines"; report the omitted byte span. */
            char gap_s[16];
            format_byte_size(gap_s, sizeof(gap_s), gap_bytes);
            marker = xasprintf("... [output truncated: omitted %s mid-line; full output saved to "
                               "%s] ...\n",
                               gap_s, cap->path);
        }
        append_clean_marker(body, marker);
        append_clean(body, tail_raw.data, tail_raw.len);
    } else {
        /* No usable head (a single unterminated line longer than the head
         * slice) or no gap between the windows: fall back to a tail-only
         * end marker. Re-read the tail at the FULL budget rather than emit
         * the reduced 7/8 slice we read above — otherwise these fallback
         * cases would keep less than plain tail-only truncation did. The
         * re-read only runs on this uncommon path. */
        buf_reset(&tail_raw);
        tail_b = 0;
        tail_l = 0;
        if (read_tail_capped(cap->fd, cap->total_bytes, output_cap_bytes(), OUTPUT_CAP_LINES,
                             &tail_raw, &tail_b, &tail_l) != 0) {
            append_clean_marker(body, xasprintf("\n[output truncated: %zu lines, full output "
                                                "unavailable (spill read failed)]",
                                                total_lines));
        } else {
            char kept_b[16], total_b[16];
            format_byte_size(kept_b, sizeof(kept_b), tail_b);
            format_byte_size(total_b, sizeof(total_b), cap->total_bytes);
            append_clean(body, tail_raw.data, tail_raw.len);
            append_clean_marker(body,
                                xasprintf("\n[output truncated: last %zu of %zu lines, %s of %s; "
                                          "full output saved to %s]",
                                          tail_l, total_lines, kept_b, total_b, cap->path));
        }
    }
    buf_free(&head_raw);
    buf_free(&tail_raw);
}

/* Build the env vector handed to the child via execve. We do this in
 * the parent — *not* in the post-fork child — because hax is multi-
 * threaded (spinner, libcurl) and only async-signal-safe functions are
 * legal between fork() and exec() in that case. setenv() isn't one:
 * it can take glibc's env/malloc locks, which may have been held by
 * another thread at fork time, deadlocking the child before the shell
 * starts. The strings here are either literals or borrowed straight
 * from environ (we don't own them); only the array itself is heap-
 * allocated, so the caller frees it with a single free().
 *
 * The transform: replace specific vars with fixed values, regardless
 * of whether the parent had them set. Inherited vars we don't touch
 * pass through unchanged.
 *
 * Override rationale:
 *   - Pagers (PAGER/GIT_PAGER/MANPAGER/SYSTEMD_PAGER/GH_PAGER) → `cat`:
 *     git log/diff, gh, man, systemctl/journalctl all hand off to less
 *     by default, which then waits for `q` on a TTY. With our pipe
 *     stdout the pager would block on /dev/tty (or just print directly
 *     and confuse the model). `cat` passes through cleanly. Each tool
 *     reads its own preferred var, so missing one (e.g. MANPAGER)
 *     leaves `man printf` hanging.
 *   - Editors (EDITOR/VISUAL/GIT_EDITOR/GIT_SEQUENCE_EDITOR) →
 *     `false`: pop-up editors (git commit without -m, rebase -i,
 *     crontab -e) hang on the TTY the agent can't drive. GIT_
 *     SEQUENCE_EDITOR is git's separate hook for the rebase todo
 *     file — it falls back to GIT_EDITOR when unset, but covering it
 *     explicitly catches a parent that set it directly. `false`
 *     exits non-zero, which all of these tools treat as "edit
 *     failed, abort" — fail-CLOSED. `true` would be tempting but
 *     fail-OPEN: `git commit --amend` would
 *     silently rewrite with the old message; `rebase -i` would
 *     silently no-op through the default plan.
 *   - TERM=dumb: the most leveraged single override. Disables ninja's
 *     \r-rewriting smart-terminal mode, makes the supports-color
 *     library short-circuit to 0 (suppressing chalk/Node colors
 *     across the ecosystem), and signals "no terminal features" to
 *     curses-style tools so they fall back to plain output. Combined
 *     with our piped (non-TTY) stdout, this catches the cargo /
 *     ripgrep / fd / bat / python-rich crowd too, since they all
 *     isatty-gate before emitting color. We deliberately do NOT also
 *     set NO_COLOR=1: when something inside the subprocess (npm,
 *     playwright) sets FORCE_COLOR, Node logs "'NO_COLOR' env is
 *     ignored due to the 'FORCE_COLOR' env being set" on every run.
 *     A tool that *forces* color via its own env wouldn't have honored
 *     NO_COLOR anyway, so we lose nothing real by dropping it.
 *   - COLORTERM= (empty): some tools probe presence rather than read
 *     value. Empty wins where unset isn't possible.
 *   - GIT_TERMINAL_PROMPT=0: git fails fast on credential prompts
 *     instead of opening /dev/tty.
 *   - AI_AGENT=hax: emerging convention via the std-env npm package.
 *     vitest already auto-disables watch mode and switches reporter
 *     to `minimal` when it sees this. Other tools are joining.
 *   - PYTHONUNBUFFERED=1: critical for streaming. Without it, CPython
 *     block-buffers stdout when not on a TTY (4 KiB chunks), so a
 *     long-running Python script looks hung from the agent's side.
 */
static char **build_child_env(void)
{
    extern char **environ;
    int n = 0;
    while (environ[n])
        n++;

    struct override {
        const char *name;
        const char *kv;
    };
    static const struct override overrides[] = {
        {"PAGER", "PAGER=cat"},
        {"GIT_PAGER", "GIT_PAGER=cat"},
        {"MANPAGER", "MANPAGER=cat"},
        {"SYSTEMD_PAGER", "SYSTEMD_PAGER=cat"},
        {"GH_PAGER", "GH_PAGER=cat"},
        {"GIT_EDITOR", "GIT_EDITOR=false"},
        {"GIT_SEQUENCE_EDITOR", "GIT_SEQUENCE_EDITOR=false"},
        {"VISUAL", "VISUAL=false"},
        {"EDITOR", "EDITOR=false"},
        {"TERM", "TERM=dumb"},
        {"COLORTERM", "COLORTERM="},
        {"GIT_TERMINAL_PROMPT", "GIT_TERMINAL_PROMPT=0"},
        {"AI_AGENT", "AI_AGENT=hax"},
        {"PYTHONUNBUFFERED", "PYTHONUNBUFFERED=1"},
        /* Per-process observability, not inheritable config: a nested hax
         * truncates these paths at startup, so passing them through would
         * destroy this process's live logs and leave both writing to the
         * same file. Empty = disabled; a child can still be traced by
         * passing an explicit (different) path in the command. */
        {"HAX_TRACE", "HAX_TRACE="},
        {"HAX_TRANSCRIPT", "HAX_TRANSCRIPT="},
    };
    const size_t override_n = sizeof(overrides) / sizeof(*overrides);

    /* Depth marker for nested hax: children run one level deeper than this
     * process, and main() refuses to start past the cap — the backstop
     * against a confused model recursively spawning subagents. The parent's
     * own depth is fixed for the process lifetime, so format once. Checked
     * parse mirroring main()'s guard: malformed or negative reads as at-cap
     * so a corrupted chain still terminates (unreachable in practice —
     * main() refuses such values at startup). */
    static char depth_kv[64];
    if (!depth_kv[0]) {
        const char *d = getenv("HAX_SUBAGENT_DEPTH");
        int depth = 0;
        if (d && *d && (!parse_int(d, &depth) || depth < 0))
            depth = HAX_SUBAGENT_MAX_DEPTH;
        int child = depth >= HAX_SUBAGENT_MAX_DEPTH ? depth : depth + 1;
        snprintf(depth_kv, sizeof(depth_kv), "HAX_SUBAGENT_DEPTH=%d", child);
    }

    /* Dynamic overrides: the published selection (may be empty — see
     * tools/bash_export.h) plus the depth marker. Same replace-regardless
     * semantics as the fixed table. */
    const char *const *sel = NULL;
    size_t sel_n = bash_export_env(&sel);
    const char *extra[8];
    size_t extra_n = 0;
    for (size_t i = 0; i < sel_n && extra_n + 1 < sizeof(extra) / sizeof(*extra); i++)
        extra[extra_n++] = sel[i];
    extra[extra_n++] = depth_kv;

    /* Worst case: original env (every entry kept) + every override
     * appended fresh + NULL terminator. We may double-allocate when
     * an override replaces an inherited entry, but the slack is
     * trivial. */
    char **envp = xmalloc((size_t)(n + (int)override_n + (int)extra_n + 1) * sizeof(*envp));
    int o = 0;
    for (int i = 0; environ[i]; i++) {
        const char *e = environ[i];
        int skip = 0;
        for (size_t p = 0; p < override_n && !skip; p++) {
            size_t plen = strlen(overrides[p].name);
            if (strncmp(e, overrides[p].name, plen) == 0 && e[plen] == '=')
                skip = 1;
        }
        for (size_t p = 0; p < extra_n && !skip; p++) {
            const char *eq = strchr(extra[p], '=');
            size_t plen = (size_t)(eq - extra[p]);
            if (strncmp(e, extra[p], plen) == 0 && e[plen] == '=')
                skip = 1;
        }
        if (skip)
            continue;
        envp[o++] = (char *)e;
    }
    for (size_t p = 0; p < override_n; p++)
        envp[o++] = (char *)overrides[p].kv;
    for (size_t p = 0; p < extra_n; p++)
        envp[o++] = (char *)extra[p];
    envp[o] = NULL;
    return envp;
}

/* Runs in the forked child after stdout/stderr have been pointed at
 * the pipe write end. Stdin is re-pointed at /dev/null so commands
 * that try to read (cat with no args, git commit waiting on a
 * message, python REPL, …) get immediate EOF instead of hanging on
 * a fd the agent has no way to feed. Only async-signal-safe
 * operations between fork() and execve(), per POSIX rules for
 * forking a multithreaded process. Never returns. */
static void exec_shell_child(const char *shell, const char *argv0, const char *cmd,
                             char *const envp[])
{
    /* Close stdin first so /dev/null (when open succeeds) lands on
     * fd 0 directly — avoids a dup2 + close dance. If open somehow
     * fails (effectively unreachable, but handle it cleanly), reads
     * return EBADF, still preferable to blocking. */
    close(STDIN_FILENO);
    (void)open("/dev/null", O_RDONLY);
    char *const argv[] = {(char *)argv0, (char *)"-c", (char *)cmd, NULL};
    execve(shell, argv, (char *const *)envp);
    _exit(127);
}

/* Stream the trailing suffix (binary marker, footer, or "(no output)")
 * through emit_display at the end of a streamed run. The body was already
 * streamed live; this only writes what comes after, using the same
 * append_run_suffix helper as the canonical-history path so the live display
 * and history stay byte-identical past the body. The truncation marker is
 * not part of the suffix — the live display conveys truncation through the
 * renderer's own head/tail elision (see append_run_suffix). */
static void stream_suffix(tool_emit_display_fn emit_display, void *user, size_t total_bytes,
                          int has_nul, int streamed_anything, int timed_out, int interrupted,
                          long timeout_ms, int status)
{
    struct buf suf;
    buf_init(&suf);
    /* If we streamed any bytes before detecting the NUL, the renderer's
     * ctrl_strip may be parked in an unterminated escape sequence. The
     * binary marker starts with `[`, which ctrl_strip would happily
     * consume as the CSI introducer — silently swallowing the user-
     * visible message. A leading \n forces an abort (ctrl_strip's
     * is_abort accepts LF) and resets the state. Footers already start
     * with \n so they don't need the same treatment. */
    if (has_nul && streamed_anything)
        buf_append_str(&suf, "\n");
    append_run_suffix(&suf, total_bytes, has_nul, streamed_anything, timed_out, interrupted,
                      timeout_ms, status);
    if (suf.len > 0)
        emit_display(suf.data, suf.len, user);
    buf_free(&suf);
}

static char *run_shell(const char *cmd, long timeout_ms, tool_emit_display_fn emit_display,
                       void *user)
{
    /* Build the env vector and resolve the shell before fork so the
     * post-fork child only performs async-signal-safe calls (strrchr
     * for the basename isn't on the POSIX async-signal-safe list
     * either, hence argv[0] is computed here too). argv[0] is the
     * shell's basename so `$0` and ps output read as "bash"/"sh"
     * rather than an absolute path. */
    char **envp = build_child_env();
    char *shell = bash_resolve_shell();
    const char *argv0 = strrchr(shell, '/');
    argv0 = argv0 ? argv0 + 1 : shell;

    /* A single pipe carries the child's combined stdout+stderr. We
     * tried PTY-based execution to keep stdout/stderr line-buffered
     * (libc switches to line buffering when isatty(1) is true), but
     * the cost — having to faithfully render \r-rewrites, full-screen
     * redraws, and OSC sequences from build tools like ninja, vitest,
     * and cargo — turned out to be too much. Pipes are predictable.
     * Block-buffering of stdout in the child is mitigated separately:
     * `TERM=dumb`, `AI_AGENT=hax`, `CI`-shaped vars, and
     * `PYTHONUNBUFFERED=1` already cover the long tail of tools. The
     * tools that block-buffer their stdout under pipes typically
     * don't matter for an agent loop (they finish fast enough that
     * the buffer flushes on exit). */
    int pipefds[2];
    if (pipe(pipefds) < 0) {
        free(shell);
        free(envp);
        return xasprintf("pipe: %s", strerror(errno));
    }
    int reader = pipefds[0];
    int writer = pipefds[1];
    pid_t pid = fork();
    if (pid < 0) {
        close(reader);
        close(writer);
        free(shell);
        free(envp);
        return xasprintf("fork: %s", strerror(errno));
    }
    if (pid == 0) {
        /* Child: own session so kill(-pid, …) reaches descendants
         * (and so opening /dev/tty fails with ENXIO instead of finding
         * the agent's terminal — this protects against /dev/tty-prompt
         * commands like sudo, ssh, gpg). stdout/stderr go to the pipe;
         * exec_shell_child redirects stdin from /dev/null. */
        close(reader);
        setsid();
        dup2(writer, STDOUT_FILENO);
        dup2(writer, STDERR_FILENO);
        if (writer > STDERR_FILENO)
            close(writer);
        exec_shell_child(shell, argv0, cmd, envp); /* never returns */
    }
    close(writer); /* parent only reads */
    free(shell);   /* parent's copies; child got its own at fork */
    free(envp);

    long deadline = timeout_ms > 0 ? sat_add(monotonic_ms(), timeout_ms) : 0;
    long grace_ms = config_duration_ms("bash.timeout_grace");
    long grace_deadline = 0;
    int timed_out = 0;
    int interrupted = 0;
    int term_sent = 0;
    int shell_exited = 0;
    int status = 0;

    struct capture cap;
    capture_init(&cap);
    /* Clamp to BASH_BYTE_CAP_MAX so the spill threshold is always
     * reachable before the drain SIGKILL fires. Without this, a user-
     * supplied HAX_TOOL_OUTPUT_CAP greater than MAX_DRAIN_BYTES would
     * let mem grow past the drain (because the spill check never trips)
     * and finalize would silently report "fit in mem, no truncation"
     * even though the kernel killed the producer mid-stream. */
    size_t cap_bytes = output_cap_bytes();
    if (cap_bytes > (size_t)BASH_BYTE_CAP_MAX)
        cap_bytes = (size_t)BASH_BYTE_CAP_MAX;
    int has_nul = 0;
    int streamed_anything = 0;
    char chunk[4096];

    for (;;) {
        long now = monotonic_ms();

        /* User-Esc interrupt: same two-stage shutdown as the timeout path
         * (SIGTERM → grace → SIGKILL). Checked before the deadline so the
         * footer correctly attributes the cause when both fire. The flag
         * latches, so once set we don't re-enter this branch. */
        if (!term_sent && interrupt_requested()) {
            interrupted = 1;
            term_sent = 1;
            if (shell_exited)
                break;
            if (grace_ms > 0) {
                kill_descendants(pid, SIGTERM);
                grace_deadline = sat_add(now, grace_ms);
            } else {
                kill_descendants(pid, SIGKILL);
                break;
            }
        }

        /* First-stage timeout: a runaway command (slow build, hung network
         * call, infinite loop) would otherwise pin the agent forever. Send
         * SIGTERM so well-behaved commands flush output, drop locks, and
         * unwind cleanly; the loop keeps draining output during the grace
         * window. With grace disabled we go straight to SIGKILL. */
        if (!term_sent && deadline > 0 && now >= deadline) {
            timed_out = 1;
            term_sent = 1;
            if (shell_exited)
                break;
            if (grace_ms > 0) {
                kill_descendants(pid, SIGTERM);
                grace_deadline = sat_add(now, grace_ms);
            } else {
                kill_descendants(pid, SIGKILL);
                break;
            }
        }

        /* Second-stage timeout: grace expired. SIGKILL unconditionally
         * — we deferred the post-shell-exit pgroup-collapse during grace
         * (see below), so stragglers may still be alive even when
         * shell_exited is set. */
        if (term_sent && grace_deadline > 0 && now >= grace_deadline) {
            kill_descendants(pid, SIGKILL);
            break;
        }

        /* Check shell status on every iteration — a backgrounded
         * writer like `yes &` holds the pipe write end open
         * indefinitely (the shell exits, but the descendant
         * inherited the pipe), so we can't rely on poll() ever
         * timing out via EOF. Once the shell is gone, SIGKILL the
         * pgroup so the descendant releases its end and read()
         * reaches EOF. The !term_sent guard preserves the grace
         * window if it happens to matter (rare: a descendant
         * detached stdout to a file before exit — then EOF arrives
         * naturally regardless).
         *
         * waitid(WNOWAIT) peeks at the exit state without reaping, so
         * the pid stays allocated to the (now-zombie) shell for the
         * rest of the loop. That keeps every subsequent kill(-pid, …)
         * and the kill_descendants() bare-pid fallback well-defined:
         * a reaped pid can be recycled by another fork() and a kill
         * would then target an unrelated process. The post-loop
         * waitpid() does the actual reap once we're done signaling.
         * (WNOWAIT must be paired with waitid here — macOS rejects
         * it on waitpid() with EINVAL.) */
        if (!shell_exited) {
            siginfo_t info;
            info.si_pid = 0;
            int rc = waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT);
            if (rc == 0 && info.si_pid == pid) {
                shell_exited = 1;
                if (!term_sent)
                    kill_descendants(pid, SIGKILL);
            } else if (rc < 0 && errno != EINTR) {
                break;
            }
        }

        /* 10ms baseline keeps deadline checks responsive without
         * meaningful CPU cost — poll() with no fd activity blocks in
         * the kernel. The clamp below shortens it further when a
         * deadline is approaching. */
        struct pollfd pfd = {.fd = reader, .events = POLLIN};
        int poll_ms = 10;
        long active_deadline = term_sent ? grace_deadline : deadline;
        if (active_deadline > 0) {
            long remaining = active_deadline - monotonic_ms();
            if (remaining < 0)
                remaining = 0;
            if (remaining < poll_ms)
                poll_ms = (int)remaining;
        }
        int pr = poll(&pfd, 1, poll_ms);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue; /* re-check shell status / deadline */

        ssize_t r = read(reader, chunk, sizeof(chunk));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0) {
            /* EOF — every write end of the pipe is closed. SIGKILL
             * the pgroup so we don't leak descendants that detached
             * their stdout (and thus aren't holding the pipe but are
             * still alive). Motivating case: `sleep 300 >/dev/null
             * 2>&1 &` — shell forks sleep with redirected fds,
             * backgrounds, exits. The pipe drops, EOF arrives in
             * the parent before waitpid(WNOHANG) sees the zombie,
             * and without this kill sleep would outlive us. ESRCH
             * /EPERM on an empty pgroup is harmless; SIGKILL on a
             * zombie preserves the prior waitpid status.
             *
             * Note on the timeout grace window: this SIGKILL fires
             * even mid-grace (term_sent == 1). That's intentional —
             * once the pipe EOFs, no further bytes can arrive on it,
             * so deferring would only add latency without preserving
             * output. Foreground cleanup (the pytest / cargo-test
             * pattern, where the shell itself catches SIGTERM,
             * flushes through the pipe, and exits) is what the grace
             * window protects, and that path keeps the pipe open
             * until the shell finishes. `test_bash_timeout_grace_
             * allows_cleanup` covers it. */
            kill_descendants(pid, SIGKILL);
            break;
        }
        if (!has_nul && memchr(chunk, '\0', (size_t)r))
            has_nul = 1;
        /* Stream this chunk live before capturing it. Skipping streaming
         * once any chunk has had a NUL keeps the "binary output
         * suppressed" guarantee intact: bytes we suppress in display also
         * stay out of the streamed display. The capture is still updated
         * (we still want total_bytes for the binary marker) but the
         * temp file gets the binary content too — finalize unlinks it. */
        if (emit_display && !has_nul) {
            emit_display(chunk, (size_t)r, user);
            streamed_anything = 1;
        }
        capture_write(&cap, chunk, (size_t)r, cap_bytes);
        /* Runaway producer (yes, tail -f, /dev/urandom, …) — bound the
         * temp file size so a misbehaving command can't fill the disk.
         * SIGKILL unconditionally: when we're in the grace window we
         * deliberately deferred the pgroup-collapse so cleanup could
         * finish, but a runaway descendant ignoring SIGTERM can still
         * flood past the budget. ESRCH on an empty pgroup is harmless. */
        if (cap.total_bytes >= (size_t)MAX_DRAIN_BYTES) {
            kill_descendants(pid, SIGKILL);
            break;
        }
    }
    close(reader);

    /* Final reap — always blocking. The in-loop waitid(WNOWAIT) above
     * detected exit without reaping, so even when shell_exited is set
     * the pid is still a zombie that needs collecting here. If the
     * shell is somehow still running (uncommon error path that broke
     * the loop without signaling), this blocks until it exits. */
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            break;
    }

    /* Build the model-facing body (truncation marker embedded). This may
     * unlink the temp file (when output fit within caps or the capture
     * never spilled); when kept, the path is copied into the body, so the
     * order of capture_free below no longer matters for it. */
    struct buf body;
    buf_init(&body);
    build_body_and_trunc(&cap, has_nul, &body);

    if (emit_display) {
        /* Streaming path: live chunks already went through emit_display.
         * Push the trailing suffix (footers / binary marker) through it too
         * so the user sees the exit status in the dim block. Truncation is
         * conveyed by the renderer's own head/tail elision, not a marker. */
        stream_suffix(emit_display, user, cap.total_bytes, has_nul, streamed_anything, timed_out,
                      interrupted, timeout_ms, status);
    }

    struct buf out;
    buf_init(&out);
    if (body.len > 0)
        buf_append(&out, body.data, body.len);
    append_run_suffix(&out, cap.total_bytes, has_nul, body.len > 0, timed_out, interrupted,
                      timeout_ms, status);
    buf_free(&body);
    capture_free(&cap);
    return buf_steal(&out);
}

/* Resolve the timeout in ms from the JSON args, falling back to the
 * configured default. The arg is integer seconds (clean model UX); the
 * configured ceiling is in ms so tests and ops can use any duration unit.
 * The schema
 * advertises the harness ceiling, but the model can't read env vars,
 * so we silently clamp rather than reject and force a retry. Returns
 * NULL on success (with *out_ms set), or an allocated error message
 * the caller surfaces to the model. */
static char *resolve_call_timeout_ms(json_t *root, long *out_ms)
{
    json_t *jt = json_object_get(root, "timeout_seconds");
    if (!jt) {
        *out_ms = config_duration_ms("bash.timeout");
        return NULL;
    }
    if (!json_is_integer(jt))
        return xstrdup("'timeout_seconds' must be an integer");
    long secs = (long)json_integer_value(jt);
    if (secs < 1)
        return xstrdup("'timeout_seconds' must be >= 1");
    /* Saturate before multiplying — signed overflow is UB and could wrap
     * to a negative timeout_ms that silently disables the timeout. The
     * clamp below brings this back down if a max is configured. */
    long timeout_ms = secs > LONG_MAX / 1000L ? LONG_MAX : secs * 1000L;
    long max_ms = config_duration_ms("bash.timeout_max");
    if (max_ms > 0 && timeout_ms > max_ms)
        timeout_ms = max_ms;
    *out_ms = timeout_ms;
    return NULL;
}

static char *run(const char *args_json, struct tool_ctx *ctx)
{
    json_error_t jerr;
    json_t *root = json_loads(args_json ? args_json : "{}", 0, &jerr);
    if (!root)
        return xasprintf("invalid arguments: %s", jerr.text);

    const char *cmd = json_string_value(json_object_get(root, "command"));
    if (!cmd || !*cmd) {
        json_decref(root);
        return xstrdup("missing 'command' argument");
    }

    long timeout_ms = 0;
    char *err = resolve_call_timeout_ms(root, &timeout_ms);
    if (err) {
        json_decref(root);
        return err;
    }

    char *out =
        run_shell(cmd, timeout_ms, ctx ? ctx->emit_display : NULL, ctx ? ctx->emit_user : NULL);
    json_decref(root);
    return out;
}

/* Rewrite args_json to drop a redundant `cd <cwd> && ` prefix from
 * the command. Some models (notably the Qwen family) prepend this on
 * every call even when cwd already equals the target — it's harmless
 * but doubles the noise in the per-call preview line. We strip when
 * the cd target resolves *exactly* to getcwd() under the limited
 * expansion forms in bash_strip_cd_prefix, so the filesystem state
 * the suffix sees is unchanged. Returns NULL when nothing was
 * stripped, leaving the agent to use the model's original args. */
static char *bash_preprocess_args(const char *args_json)
{
    if (!args_json)
        return NULL;
    json_error_t jerr;
    json_t *root = json_loads(args_json, 0, &jerr);
    if (!root)
        return NULL;
    const char *cmd = json_string_value(json_object_get(root, "command"));
    if (!cmd) {
        json_decref(root);
        return NULL;
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        json_decref(root);
        return NULL;
    }
    size_t off = bash_strip_cd_prefix(cmd, cwd, getenv("HOME"));
    if (off == 0) {
        json_decref(root);
        return NULL;
    }
    json_object_set_new(root, "command", json_string(cmd + off));
    char *out = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return out;
}

/* Decide at dispatch time whether this call's output should be hidden
 * from the live preview. The model still sees the canonical output —
 * this is purely a display heuristic. bash_classify is conservative:
 * any redirection / subshell / unknown utility falls through to the
 * normal head+tail preview. */
static int bash_is_silent(const char *args_json)
{
    if (!args_json)
        return 0;
    json_error_t jerr;
    json_t *root = json_loads(args_json, 0, &jerr);
    if (!root)
        return 0;
    const char *cmd = json_string_value(json_object_get(root, "command"));
    int verdict = cmd ? bash_cmd_is_exploration(cmd) : 0;
    json_decref(root);
    return verdict;
}

const struct tool TOOL_BASH = {
    .def =
        {
            .name = "bash",
            .description =
                "Run a shell command via bash -c (POSIX sh -c where bash is unavailable). "
                "Returns combined stdout+stderr plus exit code.\n"
                "\n"
                "Rules:\n"
                "- Each call starts in the working directory listed under `# Environment`; "
                "`cd` does not persist across calls.\n"
                "- Follow the command preferences under `# Environment` when present.\n"
                "- Default timeout is 120s; pass `timeout_seconds` for slow commands "
                "(test suites, builds). The harness enforces a hard ceiling.",
            .parameters_schema_json =
                "{\"type\":\"object\","
                "\"properties\":{"
                "\"command\":{\"type\":\"string\","
                "\"description\":\"Shell command to run.\"},"
                "\"timeout_seconds\":{\"type\":\"integer\",\"minimum\":1,"
                "\"description\":\"Optional override of the default timeout. "
                "Use a higher value for slow builds or test suites; the harness "
                "clamps to a configured maximum.\"}"
                "},"
                "\"required\":[\"command\"]}",
            .display_arg = "command",
        },
    .run = run,
    .header_rows = 3,
    .preview_tail = 1,
    .is_silent = bash_is_silent,
    .preprocess_args = bash_preprocess_args,
};
