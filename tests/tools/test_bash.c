/* SPDX-License-Identifier: MIT */
#include "tool.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "harness.h"
#include "util.h"

static char *call_bash(const char *cmd_json_escaped)
{
    char *args = xasprintf("{\"command\":\"%s\"}", cmd_json_escaped);
    char *out = TOOL_BASH.run(args, NULL, NULL);
    free(args);
    return out;
}

/* Capturing emit_display: accumulates every chunk into a buf so tests
 * can assert what bash sent live for display. */
struct capture {
    struct buf buf;
};

static int capture_write(const char *bytes, size_t n, void *user)
{
    struct capture *c = user;
    buf_append(&c->buf, bytes, n);
    return 0;
}

static char *call_bash_streamed(const char *cmd_json_escaped, struct capture *cap)
{
    char *args = xasprintf("{\"command\":\"%s\"}", cmd_json_escaped);
    char *out = TOOL_BASH.run(args, capture_write, cap);
    free(args);
    return out;
}

static void test_bash_invalid_json(void)
{
    char *out = TOOL_BASH.run("not json", NULL, NULL);
    EXPECT(strstr(out, "invalid arguments") != NULL);
    free(out);
}

static void test_bash_missing_command(void)
{
    char *out = TOOL_BASH.run("{}", NULL, NULL);
    EXPECT(strstr(out, "missing 'command'") != NULL);
    free(out);
}

static void test_bash_stdout(void)
{
    char *out = call_bash("echo hello");
    EXPECT_STR_EQ(out, "hello\n");
    free(out);
}

static void test_bash_stderr_merged(void)
{
    char *out = call_bash("echo err 1>&2");
    EXPECT_STR_EQ(out, "err\n");
    free(out);
}

static void test_bash_exit_code(void)
{
    char *out = call_bash("false");
    EXPECT(strstr(out, "[exit 1]") != NULL);
    free(out);
}

static void test_bash_signal(void)
{
    /* Self-kill with SIGTERM. */
    char *out = call_bash("kill -TERM $$");
    EXPECT(strstr(out, "[signal 15]") != NULL);
    free(out);
}

static void test_bash_no_output(void)
{
    char *out = call_bash("true");
    EXPECT_STR_EQ(out, "(no output)");
    free(out);
}

static void test_bash_stdin_detached(void)
{
    /* `cat` with no args reads stdin; if stdin wasn't /dev/null'd, it would
     * block forever. With EOF on stdin it returns immediately and produces
     * no output. */
    time_t t0 = time(NULL);
    char *out = call_bash("cat");
    time_t elapsed = time(NULL) - t0;
    EXPECT(elapsed < 3);
    EXPECT_STR_EQ(out, "(no output)");
    free(out);
}

static void test_bash_dev_tty_does_not_hang(void)
{
    /* Programs that bypass stdin and read /dev/tty directly (sudo,
     * ssh host-key prompts, gpg passphrase prompts, …) would otherwise
     * find the agent's terminal and block on input we can't deliver.
     * The child runs setsid(), so it has no controlling terminal —
     * `open("/dev/tty")` fails with ENXIO before anything blocks.
     * Worst case here is timeout, so a tight elapsed bound is the
     * load-bearing assertion; the marker check is sanity. */
    time_t t0 = time(NULL);
    char *out = call_bash("stty sane </dev/tty 2>&1; cat /dev/tty 2>&1; echo done");
    time_t elapsed = time(NULL) - t0;
    EXPECT(elapsed < 3);
    EXPECT(strstr(out, "done") != NULL);
    free(out);
}

static void test_bash_background_job_does_not_hang(void)
{
    /* Shell exits immediately; backgrounded sleep would keep the
     * pipe write end open (and so the parent's read blocked) unless
     * pgroup cleanup collapses it. */
    time_t t0 = time(NULL);
    char *out = call_bash("echo spawned; sleep 30 &");
    time_t elapsed = time(NULL) - t0;
    EXPECT(elapsed < 3);
    EXPECT(strstr(out, "spawned") != NULL);
    free(out);
}

static void test_bash_foreground_infinite_writer_caps(void)
{
    /* `yes` never exits on its own; must be killed once OUTPUT_CAP is hit. */
    time_t t0 = time(NULL);
    char *out = call_bash("yes foo");
    time_t elapsed = time(NULL) - t0;
    EXPECT(elapsed < 3);
    EXPECT(strstr(out, "[output truncated]") != NULL);
    /* We SIGKILL the pgroup, so the shell dies by signal. */
    EXPECT(strstr(out, "[signal 9]") != NULL);
    free(out);
}

static void test_bash_timeout_kills_process_tree(void)
{
    /* A 30s sleep with HAX_BASH_TIMEOUT=30ms must be killed quickly and
     * the timeout annotation surfaced to the model. The pgroup SIGTERM
     * collapses the subprocess tree (sleep is the shell's child and
     * honors SIGTERM with default disposition). */
    setenv("HAX_BASH_TIMEOUT", "30ms", 1);
    time_t t0 = time(NULL);
    char *out = call_bash("sleep 30");
    time_t elapsed = time(NULL) - t0;
    EXPECT(elapsed < 2);
    EXPECT(strstr(out, "[timed out after 30ms]") != NULL);
    /* No bare [signal N] — the timeout supersedes it. */
    EXPECT(strstr(out, "[signal ") == NULL);
    free(out);
    unsetenv("HAX_BASH_TIMEOUT");
}

static void test_bash_per_call_timeout_overrides_env(void)
{
    /* Env timeout (10ms) would fire on this 50ms sleep, but the per-call
     * `timeout_seconds=60` arg overrides it — so the sleep finishes
     * cleanly with no timeout marker. Proves the arg path is hit instead
     * of the env path. The integer-seconds floor is irrelevant here
     * because the command finishes long before the arg expires. */
    setenv("HAX_BASH_TIMEOUT", "10ms", 1);
    char *out = TOOL_BASH.run("{\"command\":\"sleep 0.05\",\"timeout_seconds\":60}", NULL, NULL);
    EXPECT(strstr(out, "[timed out") == NULL);
    free(out);
    unsetenv("HAX_BASH_TIMEOUT");
}

static void test_bash_per_call_timeout_clamped_to_max(void)
{
    /* Model asks for 9999s but max is 30ms — should clamp. */
    setenv("HAX_BASH_TIMEOUT_MAX", "30ms", 1);
    time_t t0 = time(NULL);
    char *out = TOOL_BASH.run("{\"command\":\"sleep 30\",\"timeout_seconds\":9999}", NULL, NULL);
    time_t elapsed = time(NULL) - t0;
    EXPECT(elapsed < 2);
    EXPECT(strstr(out, "[timed out after 30ms]") != NULL);
    free(out);
    unsetenv("HAX_BASH_TIMEOUT_MAX");
}

static void test_bash_timeout_graceful_sigterm(void)
{
    /* `sleep` honors SIGTERM with default disposition — it should die
     * during the grace window, not require SIGKILL escalation. */
    setenv("HAX_BASH_TIMEOUT", "30ms", 1);
    time_t t0 = time(NULL);
    char *out = call_bash("sleep 30");
    time_t elapsed = time(NULL) - t0;
    EXPECT(elapsed < 2);
    EXPECT(strstr(out, "[timed out after 30ms]") != NULL);
    free(out);
    unsetenv("HAX_BASH_TIMEOUT");
}

static void test_bash_timeout_escalates_to_sigkill(void)
{
    /* Shell traps SIGTERM (SIG_IGN) and busy-loops on builtins (no fork,
     * pgroup is just the shell). SIGTERM hits but is ignored, so we must
     * wait the grace window and escalate to SIGKILL. */
    setenv("HAX_BASH_TIMEOUT", "30ms", 1);
    setenv("HAX_BASH_TIMEOUT_GRACE", "30ms", 1);
    time_t t0 = time(NULL);
    char *out = call_bash("trap '' TERM; while :; do :; done");
    time_t elapsed = time(NULL) - t0;
    EXPECT(elapsed < 2);
    EXPECT(strstr(out, "[timed out after 30ms]") != NULL);
    free(out);
    unsetenv("HAX_BASH_TIMEOUT");
    unsetenv("HAX_BASH_TIMEOUT_GRACE");
}

static void test_bash_timeout_grace_allows_cleanup(void)
{
    /* Foreground shell traps SIGTERM and takes a moment to flush output
     * before exiting (the common pytest / npm-test / cargo-test
     * cleanup-handler pattern). The naive "deadline hit → SIGKILL"
     * path would lose that final output. The grace window defers the
     * SIGKILL so well-behaved cleanup handlers can finish. The shell
     * parses the script before launching `sleep 30`, so the trap is
     * installed before SIGTERM can arrive — no extra ASan margin
     * needed in the timeout. */
    setenv("HAX_BASH_TIMEOUT", "50ms", 1);
    setenv("HAX_BASH_TIMEOUT_GRACE", "500ms", 1);
    char *out = call_bash("trap 'sleep 0.05; echo cleaned; exit' TERM; sleep 30");
    EXPECT(strstr(out, "cleaned") != NULL);
    EXPECT(strstr(out, "[timed out") != NULL);
    free(out);
    unsetenv("HAX_BASH_TIMEOUT");
    unsetenv("HAX_BASH_TIMEOUT_GRACE");
}

static void test_bash_timeout_grace_disabled(void)
{
    /* HAX_BASH_TIMEOUT_GRACE=0 skips SIGTERM entirely and SIGKILLs at
     * the deadline — so a SIGTERM-trapping busy loop dies immediately,
     * not after a grace window. */
    setenv("HAX_BASH_TIMEOUT", "30ms", 1);
    setenv("HAX_BASH_TIMEOUT_GRACE", "0", 1);
    time_t t0 = time(NULL);
    char *out = call_bash("trap '' TERM; while :; do :; done");
    time_t elapsed = time(NULL) - t0;
    EXPECT(elapsed < 2);
    EXPECT(strstr(out, "[timed out after 30ms]") != NULL);
    free(out);
    unsetenv("HAX_BASH_TIMEOUT");
    unsetenv("HAX_BASH_TIMEOUT_GRACE");
}

static void test_bash_timeout_no_grace_short_timeout(void)
{
    /* Smallest practical timeout (1ms) with grace disabled — the
     * single-shot SIGKILL on deadline must still reach the child even
     * though setsid() in the child races with the parent's first
     * kill. The race is a POSIX-permitted ordering: if the parent
     * kills before the child setsid()'s, kill(-pid, …) fails with
     * ESRCH and the post-loop waitpid would block until the command
     * exits naturally (here, 30s of sleep). The race window is
     * microseconds and doesn't reliably reproduce on macOS, but
     * kill_descendants() in run_shell falls back to kill(pid, …) on
     * ESRCH so the path is correct under both orderings on any
     * platform. */
    setenv("HAX_BASH_TIMEOUT", "1ms", 1);
    setenv("HAX_BASH_TIMEOUT_GRACE", "0", 1);
    time_t t0 = time(NULL);
    char *out = call_bash("sleep 30");
    time_t elapsed = time(NULL) - t0;
    EXPECT(elapsed < 2);
    EXPECT(strstr(out, "[timed out after 1ms]") != NULL);
    free(out);
    unsetenv("HAX_BASH_TIMEOUT");
    unsetenv("HAX_BASH_TIMEOUT_GRACE");
}

static void test_bash_timeout_grace_no_escape_via_pipe_close(void)
{
    /* A descendant traps SIGTERM, redirects stdout/stderr away from
     * the pipe (closing its inherited write end), and runs CPU work.
     * Without the EOF-during-grace handling, the parent reads EOF and
     * returns immediately — the descendant survives. With the fix,
     * the grace-window SIGKILL collapses the pgroup. We probe the pgroup
     * directly: the subshell records its PGID via $$ (POSIX: subshell
     * inherits parent shell's $$, and the parent IS the pgroup
     * leader). The 80ms timeout gives the subshell margin to write
     * $$ and install its trap before SIGTERM arrives (matters under
     * ASan, which slows fork). */
    setenv("HAX_BASH_TIMEOUT", "80ms", 1);
    setenv("HAX_BASH_TIMEOUT_GRACE", "20ms", 1);

    char path[] = "/tmp/hax-test-pgid-XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    close(fd);

    char *cmd = xasprintf("(echo $$ > %s; "
                          "trap 'exec >/dev/null 2>&1; while :; do :; done' TERM; "
                          "sleep 30) & wait",
                          path);
    char *args = xasprintf("{\"command\":\"%s\"}", cmd);
    char *out = TOOL_BASH.run(args, NULL, NULL);
    free(args);
    free(cmd);
    EXPECT(strstr(out, "[timed out") != NULL);
    free(out);

    int pgid = -1;
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%d", &pgid) != 1)
            pgid = -1;
        fclose(f);
    }
    unlink(path);
    EXPECT(pgid > 0);

    /* With the fix, SIGKILL has already collapsed the pgroup; ESRCH or
     * EPERM (Darwin's "any members could not be signaled") arrives on
     * the first probe. Without the fix, the busy loop survives — poll
     * briefly to fail-fast, then clean up before the EXPECT. */
    int alive = 1;
    for (int i = 0; i < 20; i++) {
        if (kill(-pgid, 0) < 0 && (errno == ESRCH || errno == EPERM)) {
            alive = 0;
            break;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000000L};
        nanosleep(&ts, NULL);
    }
    if (alive)
        kill(-pgid, SIGKILL);
    EXPECT(!alive);

    unsetenv("HAX_BASH_TIMEOUT");
    unsetenv("HAX_BASH_TIMEOUT_GRACE");
}

static void test_bash_redirected_background_job_does_not_leak(void)
{
    /* `sleep N >/dev/null 2>&1 &` redirects its output away from the
     * pipe and detaches; the shell exits immediately, the last pipe
     * write reference drops, and EOF can arrive before the parent's
     * iteration-top waitpid notices the shell is gone. Without
     * explicit pgroup cleanup at EOF, the backgrounded sleep would
     * survive past our return and leak indefinitely. `nohup` is
     * belt-and-braces here: although pipes don't trigger the SIGHUP
     * cascade that PTY session-leader exit does, some shells/setups
     * do propagate SIGHUP to backgrounded jobs at exit; nohup ensures
     * sleep survives unless our explicit cleanup signals it. We
     * capture sleep's pid via $! so the test can verify the cleanup
     * fired. */
    char path[] = "/tmp/hax-test-bg-pid-XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    close(fd);

    char *cmd = xasprintf("nohup sleep 30 >/dev/null 2>&1 & echo $! > %s", path);
    char *args = xasprintf("{\"command\":\"%s\"}", cmd);
    char *out = TOOL_BASH.run(args, NULL, NULL);
    free(args);
    free(cmd);
    free(out);

    int pid = -1;
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%d", &pid) != 1)
            pid = -1;
        fclose(f);
    }
    unlink(path);
    EXPECT(pid > 0);

    /* With the fix, SIGKILL has already collapsed the pgroup; ESRCH
     * (Linux) or EPERM (Darwin, zombie-only) arrives quickly. Without
     * it, sleep would survive the full 30s. */
    int alive = 1;
    for (int i = 0; i < 20; i++) {
        if (kill(pid, 0) < 0 && (errno == ESRCH || errno == EPERM)) {
            alive = 0;
            break;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000000L};
        nanosleep(&ts, NULL);
    }
    if (alive)
        kill(pid, SIGKILL);
    EXPECT(!alive);
}

static void test_bash_timeout_huge_does_not_overflow(void)
{
    /* HAX_BASH_TIMEOUT close to LONG_MAX must not overflow the deadline
     * arithmetic. Plain builds may accidentally tolerate the wrap (the
     * `deadline > 0` guard catches the negative result and silently
     * disables the timeout) but UBSan flags the signed-overflow UB
     * directly. With saturation the deadline pegs at LONG_MAX —
     * effectively disabled, but well-defined. */
    setenv("HAX_BASH_TIMEOUT", "9223372036854775000ms", 1);
    char *out = call_bash("echo hi");
    EXPECT_STR_EQ(out, "hi\n");
    free(out);
    unsetenv("HAX_BASH_TIMEOUT");
}

static void test_bash_per_call_timeout_invalid(void)
{
    char *out = TOOL_BASH.run("{\"command\":\"true\",\"timeout_seconds\":0}", NULL, NULL);
    EXPECT(strstr(out, "'timeout_seconds' must be >= 1") != NULL);
    free(out);

    out = TOOL_BASH.run("{\"command\":\"true\",\"timeout_seconds\":-5}", NULL, NULL);
    EXPECT(strstr(out, "'timeout_seconds' must be >= 1") != NULL);
    free(out);

    out = TOOL_BASH.run("{\"command\":\"true\",\"timeout_seconds\":\"30\"}", NULL, NULL);
    EXPECT(strstr(out, "'timeout_seconds' must be an integer") != NULL);
    free(out);
}

static void test_bash_head_tail_truncation(void)
{
    /* Produce ~168 KiB — more than HEAD_CAP+TAIL_CAP (96 KiB), but well
     * under MAX_BYTES_READ (1 MiB) so the producer finishes naturally
     * and the tail ring's final bytes are preserved. HEAD and TAIL
     * markers bookend the stream so we can confirm both ends survive
     * with the middle elided in the right order. */
    char *out = call_bash("echo HEAD; seq 1 20000; echo TAIL");
    const char *p_head = strstr(out, "HEAD");
    const char *p_elided = strstr(out, "bytes elided");
    const char *p_tail = strstr(out, "TAIL");
    EXPECT(p_head != NULL);
    EXPECT(p_elided != NULL);
    EXPECT(p_tail != NULL);
    /* Strict ordering: HEAD in head buffer, elision marker between,
     * TAIL in tail ring — anything else is a bug in the assembly. */
    EXPECT(p_head < p_elided);
    EXPECT(p_elided < p_tail);
    EXPECT(strstr(out, "[output truncated]") != NULL);
    free(out);
}

static void test_bash_short_output_no_elision(void)
{
    /* Output that fits comfortably in head should not get an elision
     * marker — we'd be lying about truncation that didn't happen. */
    char *out = call_bash("for i in 1 2 3 4 5; do echo line $i; done");
    EXPECT(strstr(out, "bytes elided") == NULL);
    EXPECT(strstr(out, "[output truncated]") == NULL);
    EXPECT_STR_EQ(out, "line 1\nline 2\nline 3\nline 4\nline 5\n");
    free(out);
}

static void test_bash_caps_long_line(void)
{
    /* Single 5000-byte line (no newlines from `head -c`) gets per-line
     * capped at MAX_LINE_LEN with an inline marker. */
    char *out = call_bash("head -c 5000 /dev/zero | tr '\\\\0' x");
    EXPECT(strstr(out, "bytes elided") != NULL);
    EXPECT(strlen(out) < 2500);
    free(out);
}

static void test_bash_sanitizes_non_utf8(void)
{
    /* printf \xff produces an invalid UTF-8 byte which must be replaced.
     * Quadruple-backslash: C-literal → JSON → shell each eat one layer. */
    char *out = call_bash("printf '\\\\xff'");
    EXPECT(strstr(out, "\xEF\xBF\xBD") != NULL);
    free(out);
}

static void test_bash_binary_output_suppressed(void)
{
    /* A NUL byte in the stream marks the output binary. The body is
     * replaced with the suppression marker; no U+FFFD glyphs leak
     * through, and the exit footer (success here) is preserved. */
    char *out = call_bash("printf 'BEFORE\\\\0AFTER'");
    EXPECT(strstr(out, "[binary output suppressed:") != NULL);
    EXPECT(strstr(out, "bytes]") != NULL);
    EXPECT(strstr(out, "BEFORE") == NULL);
    EXPECT(strstr(out, "AFTER") == NULL);
    EXPECT(strstr(out, "\xEF\xBF\xBD") == NULL);
    free(out);
}

static void test_bash_binary_output_keeps_exit_footer(void)
{
    /* Binary output that exits non-zero must still surface the exit
     * code so the model can tell success from failure. */
    char *out = call_bash("printf 'BEFORE\\\\0AFTER'; exit 7");
    EXPECT(strstr(out, "[binary output suppressed:") != NULL);
    EXPECT(strstr(out, "[exit 7]") != NULL);
    free(out);
}

static void test_bash_streamed_basic(void)
{
    /* With emit_display attached, bash streams stdout chunks live AND
     * returns the canonical history. emit_display should see "hello\n"
     * (live display); the returned string should also be "hello\n". */
    struct capture cap = {0};
    buf_init(&cap.buf);
    char *out = call_bash_streamed("echo hello", &cap);
    EXPECT_STR_EQ(out, "hello\n");
    EXPECT(cap.buf.len > 0);
    EXPECT(strstr(cap.buf.data, "hello") != NULL);
    free(out);
    buf_free(&cap.buf);
}

static void test_bash_streamed_binary_history_clean(void)
{
    /* Mid-stream NUL: pre-NUL bytes are streamed live for display, but
     * the canonical history (returned string) must contain the binary
     * marker and NOT any of the body bytes. This matches the legacy
     * non-streamed behavior so the model sees the same suppression
     * either way. */
    struct capture cap = {0};
    buf_init(&cap.buf);
    char *out = call_bash_streamed("printf 'BEFORE\\\\0AFTER'; exit 0", &cap);
    /* Returned string: marker, no body bytes. */
    EXPECT(strstr(out, "[binary output suppressed:") != NULL);
    EXPECT(strstr(out, "BEFORE") == NULL);
    EXPECT(strstr(out, "AFTER") == NULL);
    /* Live display: emit_display also got the suffix at the end. The
     * pre-NUL bytes may or may not have been streamed depending on
     * whether the NUL landed in chunk 1 (printf is small enough that
     * it does — but we don't assert that, only that the marker
     * reached emit_display). */
    EXPECT(strstr(cap.buf.data, "[binary output suppressed:") != NULL);
    free(out);
    buf_free(&cap.buf);
}

static void test_bash_streamed_binary_marker_isolated_from_escape(void)
{
    /* When pre-NUL bytes were streamed, the renderer's ctrl_strip may
     * still be inside an unterminated escape sequence by the time the
     * binary marker arrives. The marker starts with `[`, which would be
     * consumed as the CSI introducer and silently swallowed. The fix
     * is for the streaming suffix to lead with \n (an abort byte for
     * ctrl_strip) so the marker always renders cleanly. Verify the
     * leading \n is in the captured emit_display stream. */
    struct capture cap = {0};
    buf_init(&cap.buf);
    /* The first printf emits an unterminated CSI introducer; the
     * second pads enough bytes to (likely) flush the kernel buffer in
     * its own read so streamed_anything becomes true before the
     * trailing NUL triggers binary suppression. ESC is encoded as
     *  so the JSON parser accepts it. */
    char *out =
        call_bash_streamed("printf '\\u001b['; sleep 0.03; printf 'pad pad pad pad\\\\0bin'", &cap);
    EXPECT(out != NULL);
    EXPECT(cap.buf.data != NULL);
    /* Marker must reach emit_display. */
    const char *marker = strstr(cap.buf.data, "[binary output suppressed:");
    EXPECT(marker != NULL);
    /* …and be preceded by \n so ctrl_strip's escape state aborts.
     * The leading \n protects the marker only when streamed_anything
     * is true (i.e., bytes ran live before the NUL was detected).
     * The sleep above is the cheapest way to force two reads. */
    EXPECT(marker != NULL && marker > cap.buf.data && marker[-1] == '\n');
    free(out);
    buf_free(&cap.buf);
}

static void test_bash_stdout_is_not_a_tty(void)
{
    /* Stdout is a plain pipe, so isatty(1) is false. Tools that gate
     * fancy output on this (progress bars, pagers, color) take the
     * non-TTY branch — exactly what we want. We tried PTY-based exec
     * to keep stdout line-buffered, but reproducing terminal behavior
     * faithfully (\r-rewrites, cursor positioning, OSC) was too much
     * complexity for too little benefit; env vars (TERM=dumb,
     * NO_COLOR, AI_AGENT, PYTHONUNBUFFERED) cover the same ground
     * with one-tenth the failure modes. */
    char *out = call_bash("[ -t 1 ] && echo TTY || echo NOTTY");
    EXPECT_STR_EQ(out, "NOTTY\n");
    free(out);
}

static void test_bash_stderr_is_not_a_tty(void)
{
    /* Stderr also pipes through the same fd; isatty(2) is false. */
    char *out = call_bash("[ -t 2 ] && echo TTY 1>&2 || echo NOTTY 1>&2");
    EXPECT_STR_EQ(out, "NOTTY\n");
    free(out);
}

static void test_bash_lf_not_crlf(void)
{
    /* Pipes don't apply ONLCR (that was a PTY artifact), so `\n`
     * reaches us as `\n` byte-for-byte. The two-line probe forces a
     * between-lines newline. */
    char *out = call_bash("printf 'a\\nb\\n'");
    EXPECT_STR_EQ(out, "a\nb\n");
    free(out);
}

static void test_bash_env_overrides(void)
{
    /* build_child_env() forces a fixed value for each var in its
     * override table, regardless of what the parent had set. Keeping
     * hax behavior independent of how the user launched it matters
     * because users invoke hax from disparate environments — a
     * developer with PAGER=less, MANPAGER=most, EDITOR=vim, FORCE_
     * COLOR=1 set in their shell rc shouldn't see the agent's bash
     * tool inherit any of that.
     *
     * The cluster of overrides has three purposes:
     *   - Pagers (PAGER/GIT_PAGER/MANPAGER/SYSTEMD_PAGER/GH_PAGER) →
     *     `cat` so commands like `git log` / `man printf` /
     *     `systemctl status` stream rather than waiting on `q`.
     *   - Editors (GIT_EDITOR/VISUAL/EDITOR) → `false` so commands
     *     that pop $EDITOR (git commit without -m, rebase -i,
     *     crontab -e) abort fail-closed instead of hanging on the
     *     TTY the agent can't drive.
     *   - Token-friendly output: TERM=dumb (kills ninja \r-rewrites
     *     and chalk colors), NO_COLOR=1 (no-color.org honored by
     *     cargo/rg/fd/git/etc.), COLORTERM= empty (presence-probing
     *     tools), AI_AGENT=hax (vitest minimal reporter), GIT_
     *     TERMINAL_PROMPT=0 (git fails fast on credential prompts),
     *     PYTHONUNBUFFERED=1 (CPython line-flushes under pipes).
     *
     * For each var: set the parent to a contradicting value, run a
     * single shell that echoes them all, and assert the full block.
     *
     * Two intentional non-overrides verified by the trailing lines:
     *   - FORCE_COLOR passes through unchanged. Setting it (any
     *     value) makes Node warn "'NO_COLOR' env is ignored due to
     *     the 'FORCE_COLOR' env being set"; NO_COLOR + TERM=dumb
     *     already cover the same ground.
     *   - MAKEFLAGS passes through so parallel builds keep their
     *     `-j` and jobserver fds.
     */
    setenv("PAGER", "less", 1);
    setenv("GIT_PAGER", "less", 1);
    setenv("MANPAGER", "less", 1);
    setenv("SYSTEMD_PAGER", "less", 1);
    setenv("GH_PAGER", "less", 1);
    setenv("GIT_EDITOR", "vim", 1);
    setenv("GIT_SEQUENCE_EDITOR", "vim", 1);
    setenv("VISUAL", "vim", 1);
    setenv("EDITOR", "vim", 1);
    setenv("TERM", "xterm-256color", 1);
    setenv("NO_COLOR", "0", 1);
    setenv("COLORTERM", "truecolor", 1);
    setenv("AI_AGENT", "other", 1);
    setenv("GIT_TERMINAL_PROMPT", "1", 1);
    setenv("PYTHONUNBUFFERED", "0", 1);
    setenv("FORCE_COLOR", "1", 1);
    setenv("MAKEFLAGS", "-j8", 1);
    char *out = call_bash("echo PAGER=$PAGER; echo GIT_PAGER=$GIT_PAGER; "
                          "echo MANPAGER=$MANPAGER; echo SYSTEMD_PAGER=$SYSTEMD_PAGER; "
                          "echo GH_PAGER=$GH_PAGER; "
                          "echo GIT_EDITOR=$GIT_EDITOR; "
                          "echo GIT_SEQUENCE_EDITOR=$GIT_SEQUENCE_EDITOR; "
                          "echo VISUAL=$VISUAL; echo EDITOR=$EDITOR; "
                          "echo TERM=$TERM; echo NO_COLOR=$NO_COLOR; echo COLORTERM=$COLORTERM; "
                          "echo AI_AGENT=$AI_AGENT; echo GIT_TERMINAL_PROMPT=$GIT_TERMINAL_PROMPT; "
                          "echo PYTHONUNBUFFERED=$PYTHONUNBUFFERED; "
                          "echo FORCE_COLOR=$FORCE_COLOR; echo MAKEFLAGS=$MAKEFLAGS");
    EXPECT_STR_EQ(out, "PAGER=cat\n"
                       "GIT_PAGER=cat\n"
                       "MANPAGER=cat\n"
                       "SYSTEMD_PAGER=cat\n"
                       "GH_PAGER=cat\n"
                       "GIT_EDITOR=false\n"
                       "GIT_SEQUENCE_EDITOR=false\n"
                       "VISUAL=false\n"
                       "EDITOR=false\n"
                       "TERM=dumb\n"
                       "NO_COLOR=1\n"
                       "COLORTERM=\n"
                       "AI_AGENT=hax\n"
                       "GIT_TERMINAL_PROMPT=0\n"
                       "PYTHONUNBUFFERED=1\n"
                       "FORCE_COLOR=1\n"
                       "MAKEFLAGS=-j8\n");
    free(out);
    unsetenv("PAGER");
    unsetenv("GIT_PAGER");
    unsetenv("MANPAGER");
    unsetenv("SYSTEMD_PAGER");
    unsetenv("GH_PAGER");
    unsetenv("GIT_EDITOR");
    unsetenv("GIT_SEQUENCE_EDITOR");
    unsetenv("VISUAL");
    unsetenv("EDITOR");
    unsetenv("TERM");
    unsetenv("NO_COLOR");
    unsetenv("COLORTERM");
    unsetenv("AI_AGENT");
    unsetenv("GIT_TERMINAL_PROMPT");
    unsetenv("PYTHONUNBUFFERED");
    unsetenv("FORCE_COLOR");
    unsetenv("MAKEFLAGS");
}

static void test_bash_streamed_history_truncated(void)
{
    /* Streamed bash history must apply the same head/tail caps as the
     * non-streamed path — emit_display can see the full output, but
     * the returned string (model history) is bounded so a busy command
     * doesn't blow the context. Use yes piped through head to produce
     * many lines deterministically. */
    struct capture cap = {0};
    buf_init(&cap.buf);
    char *out = call_bash_streamed("yes hi | head -c 150000", &cap);
    /* Truncation marker is the unambiguous signal that the legacy
     * head/tail pipeline ran. */
    EXPECT(strstr(out, "[output truncated]") != NULL);
    /* The returned string is bounded by HEAD_CAP+TAIL_CAP plus footers
     * — well under the 500 KB live stream. */
    EXPECT(strlen(out) < 200 * 1024);
    /* The streamed display must surface the same marker so the user
     * sees that the live preview understated the gap (the renderer's
     * elision marker reports captured-but-not-shown bytes; the bash
     * cap goes further). */
    EXPECT(strstr(cap.buf.data, "[output truncated]") != NULL);
    free(out);
    buf_free(&cap.buf);
}

int main(void)
{
    test_bash_invalid_json();
    test_bash_missing_command();
    test_bash_stdout();
    test_bash_stderr_merged();
    test_bash_exit_code();
    test_bash_signal();
    test_bash_no_output();
    test_bash_stdin_detached();
    test_bash_dev_tty_does_not_hang();
    test_bash_background_job_does_not_hang();
    test_bash_foreground_infinite_writer_caps();
    test_bash_timeout_kills_process_tree();
    test_bash_per_call_timeout_overrides_env();
    test_bash_per_call_timeout_clamped_to_max();
    test_bash_timeout_graceful_sigterm();
    test_bash_timeout_escalates_to_sigkill();
    test_bash_timeout_grace_allows_cleanup();
    test_bash_timeout_grace_no_escape_via_pipe_close();
    test_bash_timeout_grace_disabled();
    test_bash_timeout_no_grace_short_timeout();
    test_bash_redirected_background_job_does_not_leak();
    test_bash_timeout_huge_does_not_overflow();
    test_bash_per_call_timeout_invalid();
    test_bash_sanitizes_non_utf8();
    test_bash_binary_output_suppressed();
    test_bash_binary_output_keeps_exit_footer();
    test_bash_streamed_basic();
    test_bash_streamed_binary_history_clean();
    test_bash_streamed_binary_marker_isolated_from_escape();
    test_bash_streamed_history_truncated();
    test_bash_stdout_is_not_a_tty();
    test_bash_stderr_is_not_a_tty();
    test_bash_env_overrides();
    test_bash_lf_not_crlf();
    test_bash_head_tail_truncation();
    test_bash_short_output_no_elision();
    test_bash_caps_long_line();
    T_REPORT();
}
