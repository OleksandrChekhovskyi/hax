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
#include <sys/wait.h>
#include <unistd.h>

#include "cmd_classify.h"
#include "interrupt.h"
#include "utf8_sanitize.h"
#include "util.h"

/* Output budget split between a head buffer (preserved verbatim from the
 * start) and a tail ring (overwriting; preserves the most recent bytes).
 * For build/test failures the tail is where the actionable info lives; for
 * `find`-style listings the head is. Keeping both gives the model context
 * either way. MAX_BYTES_READ caps the total number of bytes the reader is
 * willing to drain — a runaway producer like `yes` is killed at this
 * point, so the tail stays bounded in time as well as size. */
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

/* Build the final tool result from the captured head/tail ring and exit
 * metadata. Consumes `head` (calls buf_free on it). */
static char *format_run_output(struct buf *head, const char *tail, size_t tail_pos,
                               int tail_wrapped, size_t total_bytes, int has_nul, int timed_out,
                               int interrupted, long timeout_ms, int status)
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
        struct buf raw;
        buf_init(&raw);
        truncated = assemble_head_tail(&raw, head, tail, tail_pos, tail_wrapped, total_bytes);

        /* Cap pathologically long lines (single-line minified output, log
         * lines with no newlines) before sanitizing — cap_line_lengths cuts
         * at a byte boundary which can split a multi-byte UTF-8 codepoint;
         * sanitize_utf8 then replaces the orphaned bytes with U+FFFD so the
         * final string is always valid UTF-8 (jansson rejects invalid UTF-8
         * in json_string). */
        size_t capped_len = 0;
        char *capped =
            cap_line_lengths(raw.data ? raw.data : "", raw.len, MAX_LINE_LEN, &capped_len);
        buf_free(&raw);

        char *clean = sanitize_utf8(capped, capped_len);
        free(capped);
        buf_append_str(&out, clean);
        free(clean);
    }
    buf_free(head);

    append_run_suffix(&out, total_bytes, has_nul, truncated, out.len > 0, timed_out, interrupted,
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

    struct buf head;
    buf_init(&head);
    char tail[TAIL_CAP];
    size_t tail_pos = 0;
    int tail_wrapped = 0;
    size_t total_bytes = 0;
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
        size_t consumed = 0;
        if (head.len < HEAD_CAP) {
            size_t room = HEAD_CAP - head.len;
            size_t take = (size_t)r < room ? (size_t)r : room;
            buf_append(&head, chunk, take);
            consumed = take;
        }
        while (consumed < (size_t)r) {
            size_t avail = (size_t)r - consumed;
            size_t room = TAIL_CAP - tail_pos;
            size_t take = avail < room ? avail : room;
            memcpy(tail + tail_pos, chunk + consumed, take);
            tail_pos += take;
            if (tail_pos == TAIL_CAP) {
                tail_pos = 0;
                tail_wrapped = 1;
            }
            consumed += take;
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

    if (emit_display) {
        /* Streaming path: live chunks already went through emit_display.
         * Push the trailing suffix through it too so the user sees it
         * in the dim block. The canonical history is still produced by
         * format_run_output below — it applies the same head/tail
         * truncation, line-length cap, UTF-8 sanitization, and binary-
         * suppression as the legacy path, which the live stream skips.
         * The model's view of the output is therefore the bounded
         * summary, not the unbounded live stream. */
        size_t tail_len = tail_wrapped ? TAIL_CAP : tail_pos;
        int truncated_streamed = total_bytes > head.len + tail_len;
        stream_suffix(emit_display, user, total_bytes, has_nul, streamed_anything,
                      truncated_streamed, timed_out, interrupted, timeout_ms, status);
    }

    return format_run_output(&head, tail, tail_pos, tail_wrapped, total_bytes, has_nul, timed_out,
                             interrupted, timeout_ms, status);
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
    .header_rows = 3,
    .preview_tail = 1,
    .is_silent = bash_is_silent,
};
