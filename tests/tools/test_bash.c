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
     * ssh host-key prompts, gpg passphrase prompts, …) would block
     * indefinitely on the slave PTY with no way for the agent to
     * deliver input. run_shell skips TIOCSCTTY when wiring up the
     * pty, so the child has no controlling terminal and /dev/tty
     * opens fail with ENXIO instead. The first command's
     * tcsetattr/stty variant covers the case where a child resets
     * termios via /dev/tty (which would clobber a VMIN=0 defense)
     * — the open fails before any termios manipulation happens.
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
    /* Shell exits immediately; backgrounded sleep would keep the pty
     * slave open (and so the master readable) unless pgroup cleanup
     * releases it. */
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
     * needed in the timeout.
     *
     * Scope: this is the only grace-cleanup pattern the bash tool
     * preserves under PTY. Backgrounded-subshell cleanup that flushes
     * AFTER the outer shell exits — which the pre-PTY pipe path
     * supported — can't be captured because session-leader exit both
     * triggers a SIGHUP cascade to the foreground pgroup and (on
     * macOS) revokes the slave. See the EOF branch in run_shell for
     * the full reasoning. */
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
     * though forkpty's setsid() in the child races with the parent's
     * first kill. The race is a POSIX-permitted ordering: if the
     * parent kills before the child setsid()'s, kill(-pid, …) fails
     * with ESRCH and the post-loop waitpid would block until the
     * command exits naturally (here, 30s of sleep). The race window
     * is microseconds and doesn't reliably reproduce on macOS, but
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
     * the slave (closing its references), and runs CPU work. Without
     * the EOF-during-grace handling, the parent reads EOF and returns
     * immediately — the descendant survives. With the fix, the grace-
     * window SIGKILL collapses the pgroup. We probe the pgroup
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
     * slave and detaches; the shell exits immediately, the last slave
     * reference drops, and EOF can arrive before the parent's
     * iteration-top waitpid notices the shell is gone. Without
     * explicit pgroup cleanup at EOF, the backgrounded sleep would
     * survive past our return and leak indefinitely. `nohup` makes
     * sleep ignore SIGHUP — without that, the kernel's controlling-
     * terminal SIGHUP cascade (sent when the session leader / shell
     * exits under PTY) would kill sleep on its own and mask whether
     * our explicit pgroup-cleanup is doing the work. We capture
     * sleep's pid via $! so the test can verify the cleanup fired. */
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
    /* Produce ~108 KiB — more than HEAD_CAP+TAIL_CAP (96 KiB), but
     * well under MAX_BYTES_READ (1 MiB) so the producer finishes
     * naturally and the tail ring's final bytes are preserved. HEAD
     * and TAIL markers bookend the stream so we can confirm both ends
     * survive with the middle elided in the right order. */
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

static void test_bash_stdout_is_a_tty(void)
{
    /* The whole point of forkpty: child sees stdout as a TTY, so libc
     * line-buffers and tools like grep/awk/python flush per-line in the
     * pipeline downstream. `[ -t 1 ]` is the smallest portable probe. */
    char *out = call_bash("[ -t 1 ] && echo TTY || echo NOTTY");
    EXPECT_STR_EQ(out, "TTY\n");
    free(out);
}

static void test_bash_stderr_is_a_tty(void)
{
    /* Stderr too, so colorized error output (cargo, rustc, clang) lands
     * the same way as in a real terminal. */
    char *out = call_bash("[ -t 2 ] && echo TTY 1>&2 || echo NOTTY 1>&2");
    EXPECT_STR_EQ(out, "TTY\n");
    free(out);
}

static void test_bash_interactive_helpers_neutralized(void)
{
    /* With a real TTY, common commands hand control off to interactive
     * helpers — pagers (git log/diff, man, systemctl) or editors (git
     * commit / rebase -i without -m, crontab -e). Both block forever
     * waiting for input the agent can't deliver. The bash tool
     * forces pagers to `cat` (passthrough) and editors to `false`
     * (fail-closed — git treats a non-zero editor exit as "abort",
     * which keeps `git commit --amend` from silently rewriting the
     * commit with the old message and `git rebase -i` from silently
     * no-op'ing through the default plan). Override each var the
     * relevant tool consults: man prefers MANPAGER over PAGER; git
     * commit prefers GIT_EDITOR over VISUAL over EDITOR. Verify they
     * all land at the shell as expected, even when the parent has
     * them set to something blocking. */
    setenv("MANPAGER", "less", 1);
    setenv("SYSTEMD_PAGER", "less", 1);
    setenv("GIT_EDITOR", "vim", 1);
    setenv("VISUAL", "vim", 1);
    setenv("EDITOR", "vim", 1);
    char *out = call_bash("echo $PAGER; echo $GIT_PAGER; echo $MANPAGER; echo $SYSTEMD_PAGER; "
                          "echo $GIT_EDITOR; echo $VISUAL; echo $EDITOR");
    EXPECT_STR_EQ(out, "cat\ncat\ncat\ncat\nfalse\nfalse\nfalse\n");
    free(out);
    unsetenv("MANPAGER");
    unsetenv("SYSTEMD_PAGER");
    unsetenv("GIT_EDITOR");
    unsetenv("VISUAL");
    unsetenv("EDITOR");
}

static void test_bash_term_default(void)
{
    /* TERM defaults to xterm-256color when unset (the common case in a
     * headless agent) so programs that gate colors on TERM emit them.
     * Save/restore the parent's TERM so the order of these env-touching
     * tests doesn't leak into anything that might run between them. */
    const char *saved = getenv("TERM");
    char *saved_dup = saved ? xstrdup(saved) : NULL;
    unsetenv("TERM");
    char *out = call_bash("echo $TERM");
    EXPECT_STR_EQ(out, "xterm-256color\n");
    free(out);
    if (saved_dup) {
        setenv("TERM", saved_dup, 1);
        free(saved_dup);
    }
}

static void test_bash_term_user_override_preserved(void)
{
    /* User-set TERM passes through — setenv with overwrite=0 must not
     * clobber it. Otherwise user choice (TERM=dumb, TERM=screen, …)
     * would be silently lost. */
    const char *saved = getenv("TERM");
    char *saved_dup = saved ? xstrdup(saved) : NULL;
    setenv("TERM", "dumb", 1);
    char *out = call_bash("echo $TERM");
    EXPECT_STR_EQ(out, "dumb\n");
    free(out);
    if (saved_dup) {
        setenv("TERM", saved_dup, 1);
        free(saved_dup);
    } else {
        unsetenv("TERM");
    }
}

static void test_bash_lf_not_crlf(void)
{
    /* PTY default termios maps LF→CRLF on output (ONLCR). With OPOST
     * cleared via make_raw_termios, child's `\n` reaches us as `\n`,
     * not `\r\n` — preserves byte-for-byte fidelity in the captured
     * history. The two-line probe forces a between-lines newline. */
    char *out = call_bash("printf 'a\\nb\\n'");
    EXPECT_STR_EQ(out, "a\nb\n");
    free(out);
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
     * — well under the 150 KB live stream. */
    EXPECT(strlen(out) < 120 * 1024);
    /* The streamed display must surface the same marker so the user
     * sees that the live preview understated the gap (the renderer's
     * elision marker reports captured-but-not-shown bytes; the bash
     * cap goes further). */
    EXPECT(strstr(cap.buf.data, "[output truncated]") != NULL);
    free(out);
    buf_free(&cap.buf);
}

static void test_bash_collapses_cr_rewrites_for_model(void)
{
    /* ninja/meson-style progress: \r-overprinted snapshots end with the
     * final state, all preceding states discarded. Exact bytes-equal
     * because term_lite runs before cap_line_lengths and UTF-8
     * sanitization. Quadruple-backslash: C-literal → JSON → shell. */
    char *out = call_bash("printf '[1/3] a\\\\r[2/3] bb\\\\r[3/3] ccc\\\\n'");
    EXPECT_STR_EQ(out, "[3/3] ccc\n");
    free(out);
}

static void test_bash_strips_ansi_color_for_model(void)
{
    /* Color escapes should be stripped from the model-side output —
     * they're terminal scaffolding, not signal. */
    char *out = call_bash("printf '\\\\033[31mred\\\\033[0m text\\\\n'");
    EXPECT_STR_EQ(out, "red text\n");
    free(out);
}

static void test_bash_applies_backspace_for_model(void)
{
    /* Curl/apt-style backspace erases. The model sees the post-erase
     * frame, not the raw byte stream. */
    char *out = call_bash("printf 'abcdef\\\\b\\\\b\\\\bXYZ\\\\n'");
    EXPECT_STR_EQ(out, "abcXYZ\n");
    free(out);
}

static void test_bash_crlf_collapses_for_model(void)
{
    /* CRLF is a line break, not an empty-line + content pair. Real
     * terminals render \r\n as one row break; we should too. */
    char *out = call_bash("printf 'one\\\\r\\\\ntwo\\\\r\\\\n'");
    EXPECT_STR_EQ(out, "one\ntwo\n");
    free(out);
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
    test_bash_collapses_cr_rewrites_for_model();
    test_bash_strips_ansi_color_for_model();
    test_bash_applies_backspace_for_model();
    test_bash_crlf_collapses_for_model();
    test_bash_stdout_is_a_tty();
    test_bash_stderr_is_a_tty();
    test_bash_interactive_helpers_neutralized();
    test_bash_term_default();
    test_bash_term_user_override_preserved();
    test_bash_lf_not_crlf();
    test_bash_head_tail_truncation();
    test_bash_short_output_no_elision();
    test_bash_caps_long_line();
    T_REPORT();
}
