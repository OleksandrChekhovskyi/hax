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

#include "tool.h"
#include "util.h"
#include "system/path.h"
#include "terminal/interrupt.h"
#include "text/utf8.h"
#include "text/utf8_sanitize.h"
#include "tools/bash_classify.h"

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
#define MAX_DRAIN_BYTES             (16L * 1024 * 1024)
#define BASH_BYTE_CAP_MAX           (MAX_DRAIN_BYTES - 64L * 1024)
#define BASH_TIMEOUT_DEFAULT_MS     (120L * 1000L)
#define BASH_TIMEOUT_MAX_DEFAULT_MS (1800L * 1000L)
#define BASH_GRACE_DEFAULT_MS       (2L * 1000L)

/* Read a duration env var (ms/s/m/h suffix or bare seconds). 0 disables
 * the guard; unset or unparseable falls back to `fallback_ms`. */
static long parse_timeout_env_ms(const char *name, long fallback_ms)
{
    const char *s = getenv(name);
    if (!s || !*s)
        return fallback_ms;
    long v = parse_duration_ms(s);
    return v < 0 ? fallback_ms : v;
}

static long resolve_default_timeout_ms(void)
{
    return parse_timeout_env_ms("HAX_BASH_TIMEOUT", BASH_TIMEOUT_DEFAULT_MS);
}

static long resolve_max_timeout_ms(void)
{
    return parse_timeout_env_ms("HAX_BASH_TIMEOUT_MAX", BASH_TIMEOUT_MAX_DEFAULT_MS);
}

static long resolve_grace_ms(void)
{
    return parse_timeout_env_ms("HAX_BASH_TIMEOUT_GRACE", BASH_GRACE_DEFAULT_MS);
}

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
 * exec'd /bin/sh yet, so there are no descendants to leak.
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
 * (tools run on the agent loop), so no locking. */
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

/* Read the last `cap_bytes` of the spilled file into `out`, then align
 * the front to a line boundary so we never start mid-line — except when
 * the entire window contains no newline (a single-line output > cap),
 * in which case we keep the bytes raw and let cap_line_lengths emit the
 * inline elision marker downstream. Then trim the front so at most
 * OUTPUT_CAP_LINES newline-terminated lines remain. Reports the kept-
 * byte and kept-line counts via out params (not derivable from `out->len`
 * alone because the caller wants pre-cap_line_lengths/pre-sanitize
 * numbers in the hint). */
static int read_tail_capped(int fd, size_t total_bytes, size_t cap_bytes, struct buf *out,
                            size_t *kept_bytes_out, size_t *kept_lines_out)
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

    /* Trim the front so at most OUTPUT_CAP_LINES lines remain. Count
     * newlines in the buffer; if the count is over budget, advance past
     * the first (count - cap) newlines and keep the rest. */
    size_t total_lines = count_newlines(out->data ? out->data : "", out->len);
    /* A trailing line without a final newline counts too. */
    size_t lines_in_buf = total_lines + (out->len > 0 && out->data[out->len - 1] != '\n' ? 1 : 0);
    if (lines_in_buf > OUTPUT_CAP_LINES) {
        size_t to_skip = lines_in_buf - OUTPUT_CAP_LINES;
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
        lines_in_buf = OUTPUT_CAP_LINES;
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

/* Truncation summary — computed in build_body_and_trunc and passed
 * forward so stream_suffix can render the same marker live.
 * truncated=0 means no marker is emitted. path is borrowed (NULL when
 * capture didn't preserve a file). */
struct trunc_info {
    int truncated;
    size_t kept_bytes;
    size_t kept_lines;
    size_t total_bytes;
    size_t total_lines;
    const char *path;
};

/* Append the trailing portion that follows whatever body has already
 * been written to `out`:
 *   - binary marker (when has_nul; takes precedence over truncated)
 *   - "[output truncated: ...; full output saved to PATH]" (non-binary
 *     case, bytes were dropped)
 *   - exit/timeout/interrupt footer
 *   - "(no output)" fallback when nothing else was appended in this
 *     call AND no body was produced (body_present=0)
 *
 * Used by both the canonical-history assembly (build_body_and_trunc +
 * the run_shell tail) and the live-display path (stream_suffix). When
 * for_display=1 the truncated marker is shortened to a single sub-100-
 * col line: bytes and the saved-to path are dropped because the human
 * scrolling the preview can't act on them (only the model can re-read
 * the spilled file). Footers and "(no output)" are identical in both
 * forms — they're already short and equally useful either way. */
static void append_run_suffix(struct buf *out, const struct trunc_info *t, int has_nul,
                              int body_present, int timed_out, int interrupted, long timeout_ms,
                              int status, int for_display)
{
    size_t before = out->len;
    if (has_nul) {
        char total_b[16];
        format_byte_size(total_b, sizeof(total_b), t->total_bytes);
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "[binary output suppressed: %s]", total_b);
        buf_append_str(out, tmp);
    } else if (t->truncated && for_display) {
        char *marker =
            xasprintf("\n[output truncated: last %zu of %zu lines]", t->kept_lines, t->total_lines);
        buf_append_str(out, marker);
        free(marker);
    } else if (t->truncated) {
        char kept_b[16], total_b[16];
        format_byte_size(kept_b, sizeof(kept_b), t->kept_bytes);
        format_byte_size(total_b, sizeof(total_b), t->total_bytes);
        /* Allocate dynamically — t->path can be arbitrarily long when
         * the user has a deep $TMPDIR, and a fixed-size buffer would
         * silently truncate the saved-to path the model needs to read. */
        char *marker;
        if (t->path) {
            marker = xasprintf("\n[output truncated: last %zu of %zu lines, %s of %s; "
                               "full output saved to %s]",
                               t->kept_lines, t->total_lines, kept_b, total_b, t->path);
        } else {
            /* Spill failed — file isn't available; tell the model what we
             * have and let it re-run with grep/head/tail if it needs more. */
            marker = xasprintf("\n[output truncated: last %zu of %zu lines, %s of %s; "
                               "full output unavailable (temp file write failed)]",
                               t->kept_lines, t->total_lines, kept_b, total_b);
        }
        /* Path bytes come from $TMPDIR + mkstemp's hex suffix. POSIX paths
         * are byte sequences with no UTF-8 guarantee — a Linux user with
         * non-UTF-8 locale can have arbitrary bytes in TMPDIR. Without
         * sanitization those bytes would land in the tool result, and
         * jansson would reject them on the next request, dropping the
         * result or breaking the conversation. */
        char *clean = sanitize_utf8(marker, strlen(marker));
        free(marker);
        buf_append_str(out, clean);
        free(clean);
    }
    append_footers(out, timed_out, interrupted, timeout_ms, status);
    if (out->len == before && !body_present)
        buf_append_str(out, "(no output)");
}

/* Decide whether the captured output needs truncation, build the body
 * (sanitized, line-width-capped), populate `t` for the suffix marker, and
 * unlink the temp file when it isn't needed. The caller still owns
 * `cap`; this only reads from it (and may unlink the spilled file). */
static void build_body_and_trunc(struct capture *cap, int has_nul, struct buf *body,
                                 struct trunc_info *t)
{
    t->truncated = 0;
    t->kept_bytes = 0;
    t->kept_lines = 0;
    t->total_bytes = cap->total_bytes;
    /* count_newlines only counts '\n' terminators; if the producer's
     * final line was unterminated, add it so the marker's "of N lines"
     * reflects the human-visible line count. */
    t->total_lines = cap->total_lines + (cap->trails_no_nl ? 1 : 0);
    t->path = NULL;

    if (has_nul) {
        capture_unlink(cap);
        return;
    }

    struct buf raw;
    buf_init(&raw);
    if (!cap->spilled) {
        /* Whole output fit in mem; no truncation. */
        if (cap->mem.len > 0)
            buf_append(&raw, cap->mem.data, cap->mem.len);
    } else if (cap->fd >= 0 && !cap->write_failed) {
        /* Spill succeeded; read the tail from disk, line-aligned,
         * line-count-capped. */
        size_t kept_b = 0, kept_l = 0;
        if (read_tail_capped(cap->fd, cap->total_bytes, output_cap_bytes(), &raw, &kept_b,
                             &kept_l) == 0) {
            t->truncated = 1;
            t->kept_bytes = kept_b;
            t->kept_lines = kept_l;
            t->path = cap->path; /* keep the file around for re-reads */
        } else {
            /* read_tail_capped failed — surface the truncation marker
             * with no path (write_failed branch in append_run_suffix). */
            t->truncated = 1;
            capture_unlink(cap);
        }
    } else {
        /* Spill attempted but the temp file isn't usable (mkstemp failed
         * or write_all failed mid-flush). capture_spill kept c->mem alive
         * in the mkstemp-failure case so we can still serve the pre-spill
         * prefix as the body — the model gets *something* useful instead
         * of an empty marker. write_all-failure leaves c->mem already
         * freed, in which case mem.len==0 and we fall through to an empty
         * body — partial file content is unsafe to recover. */
        t->truncated = 1;
        if (cap->mem.len > 0) {
            buf_append(&raw, cap->mem.data, cap->mem.len);
            t->kept_bytes = cap->mem.len;
            t->kept_lines = count_newlines(cap->mem.data, cap->mem.len) +
                            (cap->mem.data[cap->mem.len - 1] != '\n' ? 1 : 0);
        }
        capture_unlink(cap);
    }

    /* Cap pathologically long lines (single-line minified output, log
     * lines with no newlines) before sanitizing — cap_line_lengths cuts
     * at a byte boundary which can split a multi-byte UTF-8 codepoint;
     * sanitize_utf8 then replaces the orphaned bytes with U+FFFD so the
     * final string is always valid UTF-8 (jansson rejects invalid UTF-8
     * in json_string). */
    size_t capped_len = 0;
    char *capped =
        cap_line_lengths(raw.data ? raw.data : "", raw.len, OUTPUT_CAP_LINE_WIDTH, &capped_len);
    buf_free(&raw);

    char *clean = sanitize_utf8(capped, capped_len);
    free(capped);
    buf_append_str(body, clean);
    free(clean);

    if (!t->truncated)
        capture_unlink(cap);
}

/* Build the env vector handed to the child via execve. We do this in
 * the parent — *not* in the post-fork child — because hax is multi-
 * threaded (spinner, libcurl) and only async-signal-safe functions are
 * legal between fork() and exec() in that case. setenv() isn't one:
 * it can take glibc's env/malloc locks, which may have been held by
 * another thread at fork time, deadlocking the child before /bin/sh
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
    };
    const size_t override_n = sizeof(overrides) / sizeof(*overrides);

    /* Worst case: original env (every entry kept) + every override
     * appended fresh + NULL terminator. We may double-allocate when
     * an override replaces an inherited entry, but the slack is
     * trivial. */
    char **envp = xmalloc((size_t)(n + (int)override_n + 1) * sizeof(*envp));
    int o = 0;
    for (int i = 0; environ[i]; i++) {
        const char *e = environ[i];
        int skip = 0;
        for (size_t p = 0; p < override_n; p++) {
            size_t plen = strlen(overrides[p].name);
            if (strncmp(e, overrides[p].name, plen) == 0 && e[plen] == '=') {
                skip = 1;
                break;
            }
        }
        if (skip)
            continue;
        envp[o++] = (char *)e;
    }
    for (size_t p = 0; p < override_n; p++)
        envp[o++] = (char *)overrides[p].kv;
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
static void exec_shell_child(const char *cmd, char *const envp[])
{
    /* Close stdin first so /dev/null (when open succeeds) lands on
     * fd 0 directly — avoids a dup2 + close dance. If open somehow
     * fails (effectively unreachable, but handle it cleanly), reads
     * return EBADF, still preferable to blocking. */
    close(STDIN_FILENO);
    (void)open("/dev/null", O_RDONLY);
    char *const argv[] = {(char *)"sh", (char *)"-c", (char *)cmd, NULL};
    execve("/bin/sh", argv, (char *const *)envp);
    _exit(127);
}

/* Stream the trailing suffix (binary/truncated marker, footer, or
 * "(no output)") through emit_display at the end of a streamed run.
 * The body was already streamed live; this only writes what comes
 * after, using the same append_run_suffix helper as the canonical-
 * history path so the live display and history stay byte-identical
 * past the body. */
static void stream_suffix(tool_emit_display_fn emit_display, void *user, const struct trunc_info *t,
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
     * is_abort accepts LF) and resets the state. Footers and the
     * truncated marker already start with \n so they don't need the
     * same treatment. */
    if (has_nul && streamed_anything)
        buf_append_str(&suf, "\n");
    append_run_suffix(&suf, t, has_nul, streamed_anything, timed_out, interrupted, timeout_ms,
                      status, 1);
    if (suf.len > 0)
        emit_display(suf.data, suf.len, user);
    buf_free(&suf);
}

static char *run_shell(const char *cmd, long timeout_ms, tool_emit_display_fn emit_display,
                       void *user)
{
    /* Build the env vector before fork so the post-fork child doesn't
     * have to call non-async-signal-safe setenv. */
    char **envp = build_child_env();

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
        free(envp);
        return xasprintf("pipe: %s", strerror(errno));
    }
    int reader = pipefds[0];
    int writer = pipefds[1];
    pid_t pid = fork();
    if (pid < 0) {
        close(reader);
        close(writer);
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
        exec_shell_child(cmd, envp); /* never returns */
    }
    close(writer); /* parent only reads */
    free(envp);    /* parent's copy; child got its own at fork */

    long deadline = timeout_ms > 0 ? sat_add(monotonic_ms(), timeout_ms) : 0;
    long grace_ms = resolve_grace_ms();
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

    /* Build the model-facing body and the truncation summary. This may
     * unlink the temp file (when output fit within caps or the capture
     * never spilled). When kept, t.path borrows from cap.path — we must
     * not free cap until after stream_suffix runs. */
    struct buf body;
    buf_init(&body);
    struct trunc_info t;
    build_body_and_trunc(&cap, has_nul, &body, &t);

    if (emit_display) {
        /* Streaming path: live chunks already went through emit_display.
         * Push the trailing suffix through it too so the user sees the
         * truncation marker (with the saved-to path) and the footer in
         * the dim block. The canonical history below adds the same
         * suffix to the model's view, which is a bounded summary built
         * by build_body_and_trunc, not the unbounded live stream. */
        stream_suffix(emit_display, user, &t, has_nul, streamed_anything, timed_out, interrupted,
                      timeout_ms, status);
    }

    struct buf out;
    buf_init(&out);
    if (body.len > 0)
        buf_append(&out, body.data, body.len);
    append_run_suffix(&out, &t, has_nul, body.len > 0, timed_out, interrupted, timeout_ms, status,
                      0);
    buf_free(&body);
    capture_free(&cap);
    return buf_steal(&out);
}

/* Resolve the timeout in ms from the JSON args, falling back to the env
 * default. The arg is integer seconds (clean model UX); env-var max is
 * parsed in ms so tests and ops can use any duration unit. The schema
 * advertises the harness ceiling, but the model can't read env vars,
 * so we silently clamp rather than reject and force a retry. Returns
 * NULL on success (with *out_ms set), or an allocated error message
 * the caller surfaces to the model. */
static char *resolve_call_timeout_ms(json_t *root, long *out_ms)
{
    json_t *jt = json_object_get(root, "timeout_seconds");
    if (!jt) {
        *out_ms = resolve_default_timeout_ms();
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
    long max_ms = resolve_max_timeout_ms();
    if (max_ms > 0 && timeout_ms > max_ms)
        timeout_ms = max_ms;
    *out_ms = timeout_ms;
    return NULL;
}

static char *run(const char *args_json, tool_emit_display_fn emit_display, void *user)
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

    char *out = run_shell(cmd, timeout_ms, emit_display, user);
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
                "Run a shell command via /bin/sh -c. Returns combined stdout+stderr plus "
                "exit code.\n"
                "\n"
                "Rules:\n"
                "- Each call starts in <env> cwd; `cd` does not persist across calls.\n"
                "- Use the utilities listed in <env> preferred_commands instead of "
                "their older equivalents — the <env> line spells out each replacement.\n"
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
};
