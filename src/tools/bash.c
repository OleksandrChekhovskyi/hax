/* SPDX-License-Identifier: MIT */
#include "tool.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* forkpty(3) lives in <pty.h> on glibc/musl Linux and <util.h> on
 * Darwin. The symbol is in libutil on Linux (linked via `-lutil`) and
 * libSystem on macOS. */
#if defined(__linux__)
#include <pty.h>
#else
#include <util.h>
#endif

#include "cmd_classify.h"
#include "ctrl_strip.h"
#include "interrupt.h"
#include "term_lite.h"
#include "utf8_sanitize.h"
#include "util.h"

/* Output budget split between a head buffer (preserved verbatim from the
 * start) and a tail ring (overwriting; preserves the most recent bytes).
 * For build/test failures the tail is where the actionable info lives; for
 * `find`-style listings the head is. Keeping both gives the model context
 * either way.
 *
 * The caps measure *cleaned* bytes (post ctrl_strip + term_lite), not raw
 * PTY bytes — TUIs like vitest emit lots of redraw-cycle bytes that
 * collapse heavily once we interpret the cursor motion, and we want
 * truncation to fire only when the model's view is genuinely lost.
 *
 * Sizes are deliberately on the small side. A coding agent makes many
 * tool calls per turn; a 96 KiB cap (~24 K tokens) blown out to a 200 K
 * context still leaves room to reason. Larger caps would feed more
 * progress noise per call, costing tokens on every subsequent turn that
 * re-includes the history — and on a local model with a tighter context
 * window or slower tokenizer that lossless wins almost nothing. Tools
 * that produce inherently large but useful output (long file reads,
 * deep diffs) are better served by the user piping through head/grep/
 * tail than by a globally larger cap.
 *
 * MAX_BYTES_READ still caps raw bytes so a runaway producer (`yes`,
 * `tail -f`, /dev/urandom) gets SIGKILL'd before draining the system. */
#define HEAD_CAP                    (32 * 1024)
#define TAIL_CAP                    (64 * 1024)
#define MAX_BYTES_READ              (1024 * 1024)
#define MAX_LINE_LEN                2000
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

static long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
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
 * pid if the group doesn't yet exist. forkpty's child runs setsid()
 * to become a session leader (and thus its own pgroup leader with
 * pgid == pid), but that happens *after* fork() returns to the parent
 * — there's a small window where the parent can reach kill(-pid, …)
 * before the child has finished session setup. In that window the
 * pgroup pid doesn't exist and kill(-pid, …) fails with ESRCH, which
 * with HAX_BASH_TIMEOUT_GRACE=0 would let the loop break and block
 * in waitpid() until the command exits naturally. The fallback to
 * kill(pid, …) is safe in that window: the child hasn't exec'd
 * /bin/sh yet, so there are no descendants to leak.
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

/* Linearize head + tail-ring into `out`, splicing an elision marker
 * between them when bytes were dropped in the middle. The marker bounds
 * itself with newlines so it doesn't visually merge with whatever the
 * head's last partial line and the tail's first partial line happen to
 * be. Returns 1 if any bytes were elided (i.e. output is truncated). */
static int assemble_head_tail(struct buf *out, struct buf *head, const char *tail, size_t tail_pos,
                              int tail_wrapped, size_t total_bytes)
{
    if (head->data)
        buf_append(out, head->data, head->len);

    size_t tail_len = tail_wrapped ? TAIL_CAP : tail_pos;
    size_t kept = head->len + tail_len;
    size_t elided = total_bytes > kept ? total_bytes - kept : 0;

    if (elided > 0) {
        char marker[80];
        int m = snprintf(marker, sizeof(marker), "\n... [%zu bytes elided] ...\n", elided);
        buf_append(out, marker, (size_t)m);
    }

    if (tail_len > 0) {
        if (tail_wrapped) {
            buf_append(out, tail + tail_pos, TAIL_CAP - tail_pos);
            buf_append(out, tail, tail_pos);
        } else {
            buf_append(out, tail, tail_pos);
        }
    }

    return elided > 0;
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
 *   - binary marker (when has_nul; takes precedence over truncated)
 *   - "[output truncated]" (non-binary case, bytes were dropped)
 *   - exit/timeout/interrupt footer
 *   - "(no output)" fallback when nothing else was appended in this
 *     call AND no body was produced (body_present=0)
 *
 * Used by both the canonical-history assembly (format_run_output) and
 * the live-display path (stream_suffix), so the model and the user see
 * consistent suffix content. */
static void append_run_suffix(struct buf *out, size_t total_bytes, int has_nul, int truncated,
                              int body_present, int timed_out, int interrupted, long timeout_ms,
                              int status)
{
    size_t before = out->len;
    if (has_nul) {
        char tmp[80];
        snprintf(tmp, sizeof(tmp), "[binary output suppressed: %zu bytes]", total_bytes);
        buf_append_str(out, tmp);
    } else if (truncated) {
        buf_append_str(out, "\n[output truncated]");
    }
    append_footers(out, timed_out, interrupted, timeout_ms, status);
    if (out->len == before && !body_present)
        buf_append_str(out, "(no output)");
}

/* Streaming-cleanup capture context. Bytes flow: raw PTY chunk →
 * ctrl_strip → term_lite → on_clean_line (this callback) → head buf
 * for the first HEAD_CAP cleaned bytes, then tail ring for everything
 * after. total_clean tracks the running count of cleaned bytes
 * regardless of where they landed, which drives the truncation check
 * (cleaned > head + tail_len ⇒ middle bytes were dropped). */
struct clean_capture {
    struct buf *head;
    char *tail;
    size_t *tail_pos;
    int *tail_wrapped;
    size_t *total_clean;
};

static void clean_emit_byte(struct clean_capture *cap, char c)
{
    (*cap->total_clean)++;
    if (cap->head->len < HEAD_CAP) {
        buf_append(cap->head, &c, 1);
        return;
    }
    cap->tail[*cap->tail_pos] = c;
    (*cap->tail_pos)++;
    if (*cap->tail_pos == TAIL_CAP) {
        *cap->tail_pos = 0;
        *cap->tail_wrapped = 1;
    }
}

/* term_lite on_line callback: each row's bytes flow into the cleaned
 * head/tail capture. has_newline=1 contributes a trailing '\n' so the
 * captured stream reads as a normal multi-line text file. A trailing
 * partial (has_newline=0) reads as "<body>[exit N]" without a phantom
 * blank line before the footer — matches the pre-collapse layout when
 * the producer exited mid-line. */
static void on_clean_line(const char *line, size_t len, int has_newline, void *user)
{
    struct clean_capture *cap = user;
    for (size_t i = 0; i < len; i++)
        clean_emit_byte(cap, line[i]);
    if (has_newline)
        clean_emit_byte(cap, '\n');
}

/* Build the final tool result from the captured head/tail ring and exit
 * metadata. Consumes `head` (calls buf_free on it). raw_bytes counts
 * bytes off the PTY (used for the binary-suppressed marker, which
 * reports the real on-the-wire size); clean_bytes counts bytes after
 * ctrl_strip + term_lite collapse (used for truncation accounting,
 * since head/tail hold cleaned bytes). */
static char *format_run_output(struct buf *head, const char *tail, size_t tail_pos,
                               int tail_wrapped, size_t raw_bytes, size_t clean_bytes, int has_nul,
                               int timed_out, int interrupted, long timeout_ms, int status)
{
    struct buf out;
    buf_init(&out);
    int truncated = 0;

    /* Binary output: a NUL byte essentially never appears in legitimate
     * text output, but executables, archives, and images contain it
     * pervasively. Showing the user a wall of U+FFFD glyphs and feeding
     * the same to the model is pure waste — replace the body with the
     * marker (emitted by append_run_suffix below) and keep only the
     * exit/timeout footer. */
    if (!has_nul) {
        struct buf assembled;
        buf_init(&assembled);
        truncated = assemble_head_tail(&assembled, head, tail, tail_pos, tail_wrapped, clean_bytes);

        /* head/tail already passed through ctrl_strip + term_lite during
         * the read loop, so the assembled buffer is ANSI-free and
         * \r/\b/CSI-folded. Just cap pathologically long lines before
         * sanitizing — cap_line_lengths cuts at a byte boundary which
         * can split a multi-byte UTF-8 codepoint; sanitize_utf8 then
         * replaces orphaned bytes with U+FFFD so the final string is
         * always valid UTF-8 (jansson rejects invalid UTF-8 in
         * json_string). */
        size_t capped_len = 0;
        char *capped = cap_line_lengths(assembled.data ? assembled.data : "", assembled.len,
                                        MAX_LINE_LEN, &capped_len);
        buf_free(&assembled);

        char *clean = sanitize_utf8(capped, capped_len);
        free(capped);
        buf_append_str(&out, clean);
        free(clean);
    }
    buf_free(head);

    append_run_suffix(&out, raw_bytes, has_nul, truncated, out.len > 0, timed_out, interrupted,
                      timeout_ms, status);
    return buf_steal(&out);
}

/* Build the env vector handed to the child via execve. We do this in
 * the parent — *not* in the post-fork child — because hax is multi-
 * threaded (spinner, libcurl) and only async-signal-safe functions are
 * legal between fork() and exec() in that case. setenv() isn't one:
 * it can take glibc's env/malloc locks, which may have been held by
 * another thread at fork time, deadlocking the child before /bin/sh
 * starts. The strings here are either literals or borrowed straight
 * from environ (we don't own them); only the array itself is heap-
 * allocated, so the caller frees it with a single free(). */
static char **build_child_env(void)
{
    extern char **environ;
    int n = 0;
    while (environ[n])
        n++;

    /* Vars that would hand control to an interactive helper (pager or
     * editor) when stdout is a TTY — under the old pipe path those
     * branches were dead because isatty was false, but on a PTY they
     * fire and block waiting for input the agent has no way to
     * deliver:
     *   - Pagers (git log/diff, man, systemctl/journalctl) → `cat`
     *     just streams the output through. Have to cover each tool's
     *     preferred name: man prefers MANPAGER over PAGER, so
     *     overriding only the latter still hangs `man printf` for
     *     users who set MANPAGER=less.
     *   - Editors (git commit / git rebase -i / git commit --amend
     *     without -m, crontab -e, …) → `false` exits non-zero,
     *     which all the relevant tools treat as "edit failed,
     *     abort". `true` would be tempting but is fail-OPEN: an
     *     amend without -m would silently rewrite the commit with
     *     the old message, a `rebase -i` would silently no-op
     *     through the default plan. `false` is fail-CLOSED — git
     *     aborts with a clear error and the model can re-run with
     *     -m / --no-edit / explicit plan. */
    struct override {
        const char *name;
        const char *kv;
    };
    static const struct override interactive_overrides[] = {
        {"PAGER", "PAGER=cat"},
        {"GIT_PAGER", "GIT_PAGER=cat"},
        {"MANPAGER", "MANPAGER=cat"},
        {"SYSTEMD_PAGER", "SYSTEMD_PAGER=cat"},
        {"GIT_EDITOR", "GIT_EDITOR=false"},
        {"VISUAL", "VISUAL=false"},
        {"EDITOR", "EDITOR=false"},
    };
    const size_t override_n = sizeof(interactive_overrides) / sizeof(*interactive_overrides);

    /* Worst case: original env minus the vars we drop plus our
     * overrides + TERM default + NULL terminator. */
    char **envp = xmalloc((size_t)(n + (int)override_n + 2) * sizeof(*envp));
    int has_term = 0;
    int o = 0;
    for (int i = 0; environ[i]; i++) {
        const char *e = environ[i];
        int dropped = 0;
        for (size_t p = 0; p < override_n; p++) {
            size_t plen = strlen(interactive_overrides[p].name);
            if (strncmp(e, interactive_overrides[p].name, plen) == 0 && e[plen] == '=') {
                dropped = 1;
                break;
            }
        }
        if (dropped)
            continue;
        if (strncmp(e, "TERM=", 5) == 0)
            has_term = 1;
        envp[o++] = (char *)e;
    }
    for (size_t p = 0; p < override_n; p++)
        envp[o++] = (char *)interactive_overrides[p].kv;
    /* TERM default for programs that gate colors on it. The user's
     * choice wins when set — xterm-256color only fills in when
     * inherited TERM was empty. ctrl_strip handles whatever SGR /
     * CSI / OSC the resulting colors produce. */
    if (!has_term)
        envp[o++] = (char *)"TERM=xterm-256color";
    envp[o] = NULL;
    return envp;
}

/* Runs in the forkpty()'d child. forkpty has already called setsid(),
 * acquired the slave as our controlling terminal, and dup'd the slave
 * to fds 0/1/2. Stdin is then re-pointed at /dev/null so commands that
 * try to read (cat, git commit, python REPL, …) get immediate EOF
 * instead of blocking on the master — the user has no input channel
 * here. stdout/stderr stay on the slave so isatty(1)/isatty(2) report
 * true and child libc stays line-buffered (the whole point of moving
 * off pipes). Only async-signal-safe operations between forkpty() and
 * execve(), per POSIX rules for forking a multithreaded process. Never
 * returns. */
static void exec_shell_child(const char *cmd, char *const envp[])
{
    /* Close stdin first so /dev/null (when open succeeds) lands on
     * fd 0 directly — avoids a dup2 + close dance. If open somehow
     * fails (effectively unreachable, but handle it cleanly), reads
     * return EBADF, still preferable to blocking forever on the
     * slave PTY waiting for input the agent never delivers. */
    close(STDIN_FILENO);
    (void)open("/dev/null", O_RDONLY);
    char *const argv[] = {(char *)"sh", (char *)"-c", (char *)cmd, NULL};
    execve("/bin/sh", argv, (char *const *)envp);
    _exit(127);
}

/* Build the slave-side termios passed to openpty. Roughly raw mode
 * (in the spirit of cfmakeraw, which we avoid because it's a BSD
 * extension that isn't reliably exposed under our _POSIX_C_SOURCE
 * gates), tuned for our use case — a one-shot PTY whose master we
 * only read from, never write to. We zero c_cflag and OR in
 * CREAD|CS8; that encodes baud as B0 ("hang up") which the kernel
 * ignores for PTY slaves but would matter on a real serial fd, so
 * don't copy this verbatim for one. The bits that matter for hax:
 *   - OPOST off: child's `\n` reaches us as `\n`, not `\r\n` (the
 *     default ONLCR mapping would otherwise pollute every line of
 *     captured output and break byte-equality tests).
 *   - ECHO/ICANON off: belt-and-braces. We never write to the master
 *     so echo can't fire in practice, but raw mode keeps the slave
 *     well-defined if a future change ever does write input.
 *   - VMIN=0, VTIME=0: belt-and-braces against blocking slave reads.
 *     The primary defense against /dev/tty hangs is skipping
 *     TIOCSCTTY in run_shell so the slave isn't a controlling
 *     terminal at all, but if a child somehow acquires one and reads
 *     it, VMIN=0 keeps the read non-blocking. */
static void make_raw_termios(struct termios *t)
{
    memset(t, 0, sizeof *t);
    t->c_iflag = 0;
    t->c_oflag = 0;
    t->c_cflag = CREAD | CS8;
    t->c_lflag = 0;
    t->c_cc[VMIN] = 0;
    t->c_cc[VTIME] = 0;
}

/* Stream the trailing suffix (binary/truncated marker, footer, or
 * "(no output)") through emit_display at the end of a streamed run.
 * The body was already streamed live; this only writes what comes
 * after, using the same append_run_suffix helper as the canonical-
 * history path so the live display and history stay byte-identical
 * past the body. */
static void stream_suffix(tool_emit_display_fn emit_display, void *user, size_t total_bytes,
                          int has_nul, int streamed_anything, int truncated, int timed_out,
                          int interrupted, long timeout_ms, int status)
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
    append_run_suffix(&suf, total_bytes, has_nul, truncated, streamed_anything, timed_out,
                      interrupted, timeout_ms, status);
    if (suf.len > 0)
        emit_display(suf.data, suf.len, user);
    buf_free(&suf);
}

static char *run_shell(const char *cmd, long timeout_ms, tool_emit_display_fn emit_display,
                       void *user)
{
    /* Fixed window size: wide enough that `git log --graph`, build
     * progress lines, and pytest output don't column-wrap; tall enough
     * that pager-style "more?" heuristics that key off rows don't
     * trigger. hax does its own line-wrapping when rendering the dim
     * tool block, so the host terminal's actual width is irrelevant. */
    struct winsize ws = {.ws_row = 50, .ws_col = 200, .ws_xpixel = 0, .ws_ypixel = 0};
    struct termios t;
    make_raw_termios(&t);

    /* Build the env vector before fork so the post-fork child doesn't
     * have to call non-async-signal-safe setenv. */
    char **envp = build_child_env();

    /* openpty + manual fork instead of forkpty so we can deliberately
     * skip TIOCSCTTY in the child. Without that ioctl, the slave is a
     * tty device (so isatty(stdout/stderr) still reports true and the
     * line-buffering benefit holds) but is *not* the child's
     * controlling terminal — opening /dev/tty fails with ENXIO instead
     * of blocking on input the agent has no way to deliver. That
     * matters because VMIN=0 alone isn't enough: a command like
     * `stty sane </dev/tty; cat /dev/tty` calls tcsetattr to put the
     * line back into canonical mode (VMIN=1) and would then hang on
     * the read until our timeout. With no controlling tty the open
     * fails first and the whole class of /dev/tty-prompt commands
     * (sudo, ssh host-key prompt, gpg passphrase, …) fails fast. */
    int master = -1, slave = -1;
    if (openpty(&master, &slave, NULL, &t, &ws) < 0) {
        free(envp);
        return xasprintf("openpty: %s", strerror(errno));
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        close(slave);
        free(envp);
        return xasprintf("fork: %s", strerror(errno));
    }
    if (pid == 0) {
        /* Child: own session (so kill(-pid, …) reaches descendants),
         * stdout/stderr on the slave (so isatty is true and libc
         * line-buffers), but no TIOCSCTTY → no controlling tty. */
        close(master);
        setsid();
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO)
            close(slave);
        exec_shell_child(cmd, envp); /* never returns */
    }
    close(slave); /* parent only needs the master end */
    free(envp);   /* parent's copy; child got its own at fork */

    long deadline = timeout_ms > 0 ? sat_add(monotonic_ms(), timeout_ms) : 0;
    long grace_ms = resolve_grace_ms();
    long grace_deadline = 0;
    int timed_out = 0;
    int interrupted = 0;
    int term_sent = 0;
    int shell_exited = 0;
    int status = 0;

    struct buf head;
    buf_init(&head);
    /* TAIL_CAP is sized large enough that putting the ring on the heap
     * is safer than the stack — main thread has plenty of room but
     * pthread workers (libcurl, spinner) get smaller default stacks
     * and sharing the layout convention keeps things predictable. */
    char *tail = xmalloc(TAIL_CAP);
    size_t tail_pos = 0;
    int tail_wrapped = 0;
    /* total_bytes counts RAW PTY bytes — drives the binary-suppressed
     * marker (which should report on-wire size) and the runaway guard.
     * total_clean counts bytes after ctrl_strip + term_lite collapse;
     * head/tail hold these, so the truncation check is against this. */
    size_t total_bytes = 0;
    size_t total_clean = 0;
    int has_nul = 0;
    int streamed_anything = 0;
    char chunk[4096];

    /* Stream-cleanup pipeline: raw PTY bytes go through ctrl_strip
     * (drop ANSI / forward layout-affecting CSIs) and term_lite (apply
     * cursor motion / line erase / CRLF) on the fly, with the cleaned
     * line emissions captured into head/tail via on_clean_line. The
     * model sees what a real terminal would have rendered, not the
     * wire-format bytes — important for TUIs like vitest that emit
     * heavy redraw cycles whose intermediate frames don't matter. */
    struct ctrl_strip cs_clean;
    ctrl_strip_init(&cs_clean);
    struct term_lite tl_clean;
    term_lite_init(&tl_clean);
    struct clean_capture cap_ctx = {
        .head = &head,
        .tail = tail,
        .tail_pos = &tail_pos,
        .tail_wrapped = &tail_wrapped,
        .total_clean = &total_clean,
    };

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
         * writer like `yes &` keeps the slave readable indefinitely,
         * so we can't rely on poll() ever timing out. Once the shell
         * is gone, kill the process group so read() can reach EOF.
         * The !term_sent guard preserves the grace window if it
         * happens to matter (a niche where shell exit doesn't trigger
         * master EOF — e.g. a descendant that detached stdout to a
         * file). Under typical PTY semantics the EOF branch fires
         * first and SIGKILLs anyway; see the note there.
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
        struct pollfd pfd = {.fd = master, .events = POLLIN};
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

        ssize_t r = read(master, chunk, sizeof(chunk));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            /* On Linux, reading from a pty master after the slave has
             * been hung up returns -1 with errno=EIO. macOS returns 0.
             * Normalize to the EOF branch below so the rest of the
             * logic doesn't have to care. */
            if (errno == EIO)
                r = 0;
            else
                break;
        }
        if (r == 0) {
            /* EOF — slave closed in the child. SIGKILL the pgroup
             * unconditionally so we don't leak redirected/detached
             * descendants. Motivating case: `sleep 300 >/dev/null
             * 2>&1 &` — shell forks sleep with redirected fds,
             * backgrounds, exits. The slave drops, EOF arrives in
             * the parent before waitpid(WNOHANG) sees the zombie,
             * and without this kill sleep would outlive us. ESRCH
             * /EPERM on an empty pgroup is harmless; SIGKILL on a
             * zombie preserves the prior waitpid status.
             *
             * Note on the timeout grace window: this SIGKILL fires
             * even mid-grace (term_sent == 1). That's intentional —
             * once master EOFs, no further bytes can arrive on it,
             * so deferring would only add latency without preserving
             * output. This is a behavior difference from the pre-
             * PTY pipe path, where backgrounded subshells doing
             * `trap '...; echo cleaned' TERM` could keep the pipe
             * open and flush during grace. Under PTY, session-leader
             * exit triggers a SIGHUP cascade to the foreground pgroup
             * and (on macOS) revokes the slave, so that pattern can't
             * deliver output regardless of how the loop handles EOF.
             * Foreground cleanup (the pytest / cargo-test pattern,
             * where the shell itself catches SIGTERM and flushes
             * before exit) is what the grace window protects, and
             * `test_bash_timeout_grace_allows_cleanup` covers it. */
            kill_descendants(pid, SIGKILL);
            break;
        }
        total_bytes += (size_t)r;
        if (!has_nul && memchr(chunk, '\0', (size_t)r))
            has_nul = 1;
        /* Stream this chunk live before writing into head/tail buffers.
         * Skipping streaming once any chunk has had a NUL keeps the
         * "binary output suppressed" guarantee intact: bytes we suppress
         * in display also stay out of the streamed display. head/tail
         * are still populated for the no-emit_display path used by the
         * test suite. */
        if (emit_display && !has_nul) {
            emit_display(chunk, (size_t)r, user);
            streamed_anything = 1;
        }
        /* Run the chunk through ctrl_strip + term_lite to collapse
         * cursor motion / line erase / CRLF into clean line emissions,
         * captured into head/tail via on_clean_line. Skip when the
         * stream has had a NUL — binary content shouldn't drive the
         * line interpreter (it'd produce nonsense glyphs and waste
         * cycles), and append_run_suffix replaces the body with the
         * binary marker anyway. */
        if (!has_nul) {
            char clean_buf[sizeof(chunk)];
            size_t cn = ctrl_strip_feed(&cs_clean, chunk, (size_t)r, clean_buf);
            term_lite_feed(&tl_clean, clean_buf, cn, on_clean_line, &cap_ctx);
        }
        /* Runaway producer (yes, tail -f, /dev/urandom, …) — once we've
         * drained MAX_BYTES_READ, the tail ring already holds the most
         * recent TAIL_CAP bytes and reading further is wasted work.
         * SIGKILL unconditionally: when we're in the grace window we
         * deliberately deferred the pgroup-collapse so cleanup could
         * finish, but a runaway descendant ignoring SIGTERM can still
         * flood past the budget. ESRCH on an empty pgroup is harmless. */
        if (total_bytes >= MAX_BYTES_READ) {
            kill_descendants(pid, SIGKILL);
            break;
        }
    }
    close(master);

    /* Final reap — always blocking. The in-loop waitid(WNOWAIT) above
     * detected exit without reaping, so even when shell_exited is set
     * the pid is still a zombie that needs collecting here. If the
     * shell is somehow still running (uncommon error path that broke
     * the loop without signaling), this blocks until it exits. */
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            break;
    }

    /* Drain any rows still buffered in term_lite (cursor row + below)
     * into head/tail. After this, the cleaned stream is fully captured
     * and total_clean reflects the final cleaned-byte count. */
    if (!has_nul)
        term_lite_flush(&tl_clean, on_clean_line, &cap_ctx);
    term_lite_free(&tl_clean);

    if (emit_display) {
        /* Streaming path: live chunks already went through emit_display.
         * Push the trailing suffix through it too so the user sees it
         * in the dim block. The canonical history is still produced by
         * format_run_output below — it applies the same head/tail cap,
         * line-length cap, UTF-8 sanitization, and binary-suppression
         * as the legacy path, which the live stream skips. */
        size_t tail_len = tail_wrapped ? TAIL_CAP : tail_pos;
        int truncated_streamed = total_clean > head.len + tail_len;
        stream_suffix(emit_display, user, total_bytes, has_nul, streamed_anything,
                      truncated_streamed, timed_out, interrupted, timeout_ms, status);
    }

    char *result = format_run_output(&head, tail, tail_pos, tail_wrapped, total_bytes, total_clean,
                                     has_nul, timed_out, interrupted, timeout_ms, status);
    free(tail);
    return result;
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
 * this is purely a display heuristic. cmd_classify is conservative:
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
    int verdict = cmd ? cmd_is_exploration(cmd) : 0;
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
                "- Each call starts in the directory shown in <env> cwd, and that "
                "directory is fixed for the session. Run commands from there "
                "directly. To target a subdirectory, use a relative path or "
                "`(cd subdir && cmd)` in one call.\n"
                "- Use relative paths from cwd; absolute paths to files inside cwd "
                "are unnecessarily verbose. Re-`cd`-ing to the path that's already "
                "cwd is a no-op, and `cd` to anywhere else doesn't persist across "
                "calls anyway.\n"
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
    .preview_tail = 1,
    .is_silent = bash_is_silent,
};
