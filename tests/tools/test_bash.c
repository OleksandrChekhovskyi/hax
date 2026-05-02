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
    char *out = TOOL_BASH.run(args);
    free(args);
    return out;
}

static void test_bash_invalid_json(void)
{
    char *out = TOOL_BASH.run("not json");
    EXPECT(strstr(out, "invalid arguments") != NULL);
    free(out);
}

static void test_bash_missing_command(void)
{
    char *out = TOOL_BASH.run("{}");
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

static void test_bash_background_job_does_not_hang(void)
{
    /* Shell exits immediately; backgrounded sleep would keep the pipe open
     * unless pgroup cleanup releases it. */
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
    char *out = TOOL_BASH.run("{\"command\":\"sleep 0.05\",\"timeout_seconds\":60}");
    EXPECT(strstr(out, "[timed out") == NULL);
    free(out);
    unsetenv("HAX_BASH_TIMEOUT");
}

static void test_bash_per_call_timeout_clamped_to_max(void)
{
    /* Model asks for 9999s but max is 30ms — should clamp. */
    setenv("HAX_BASH_TIMEOUT_MAX", "30ms", 1);
    time_t t0 = time(NULL);
    char *out = TOOL_BASH.run("{\"command\":\"sleep 30\",\"timeout_seconds\":9999}");
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

static void test_bash_timeout_grace_allows_background_cleanup(void)
{
    /* Backgrounded subshell traps SIGTERM and takes a moment to finish
     * cleanup before exiting; the outer shell has no trap and exits
     * immediately on SIGTERM. The naive "shell exited → SIGKILL pgroup"
     * path would cut off the subshell's cleanup the moment we reap the
     * outer shell. The grace window must defer that pgroup-collapse so
     * background workers can flush. Trap delay (100ms) is comfortably
     * longer than the loop's 10ms poll cadence so the outer shell is
     * reaped before the subshell finishes — exactly the buggy timing.
     * The 80ms timeout gives the subshell margin to install its trap
     * before SIGTERM arrives (matters under ASan, which slows fork). */
    setenv("HAX_BASH_TIMEOUT", "80ms", 1);
    setenv("HAX_BASH_TIMEOUT_GRACE", "500ms", 1);
    char *out = call_bash("(trap 'sleep 0.1; echo cleaned' TERM; sleep 30) & wait");
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

static void test_bash_timeout_grace_no_escape_via_pipe_close(void)
{
    /* A descendant traps SIGTERM, redirects stdout/stderr away from the
     * pipe (closing it), and runs CPU work. Without the EOF-during-
     * grace handling, the parent reads EOF and returns immediately —
     * the descendant survives. With the fix, the grace-window SIGKILL
     * collapses the pgroup. We probe the pgroup directly: the subshell
     * records its PGID via $$ (POSIX: subshell inherits parent shell's
     * $$, and the parent IS the pgroup leader). The 80ms timeout gives
     * the subshell margin to write $$ and install its trap before
     * SIGTERM arrives (matters under ASan, which slows fork). */
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
    char *out = TOOL_BASH.run(args);
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
     * pipe and detaches; the shell exits immediately, the pipe closes,
     * and EOF can arrive before the parent's iteration-top waitpid
     * notices the shell is gone. Without explicit pgroup cleanup at
     * EOF, the backgrounded sleep would survive past our return and
     * leak indefinitely. We capture sleep's pid via $! so the test
     * can verify the cleanup actually fired. */
    char path[] = "/tmp/hax-test-bg-pid-XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    close(fd);

    char *cmd = xasprintf("sleep 30 >/dev/null 2>&1 & echo $! > %s", path);
    char *args = xasprintf("{\"command\":\"%s\"}", cmd);
    char *out = TOOL_BASH.run(args);
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
    char *out = TOOL_BASH.run("{\"command\":\"true\",\"timeout_seconds\":0}");
    EXPECT(strstr(out, "'timeout_seconds' must be >= 1") != NULL);
    free(out);

    out = TOOL_BASH.run("{\"command\":\"true\",\"timeout_seconds\":-5}");
    EXPECT(strstr(out, "'timeout_seconds' must be >= 1") != NULL);
    free(out);

    out = TOOL_BASH.run("{\"command\":\"true\",\"timeout_seconds\":\"30\"}");
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
    char *out = call_bash("echo HEAD; seq 1 30000; echo TAIL");
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
    test_bash_background_job_does_not_hang();
    test_bash_foreground_infinite_writer_caps();
    test_bash_timeout_kills_process_tree();
    test_bash_per_call_timeout_overrides_env();
    test_bash_per_call_timeout_clamped_to_max();
    test_bash_timeout_graceful_sigterm();
    test_bash_timeout_escalates_to_sigkill();
    test_bash_timeout_grace_allows_background_cleanup();
    test_bash_timeout_grace_no_escape_via_pipe_close();
    test_bash_timeout_grace_disabled();
    test_bash_redirected_background_job_does_not_leak();
    test_bash_timeout_huge_does_not_overflow();
    test_bash_per_call_timeout_invalid();
    test_bash_sanitizes_non_utf8();
    test_bash_binary_output_suppressed();
    test_bash_binary_output_keeps_exit_footer();
    test_bash_head_tail_truncation();
    test_bash_short_output_no_elision();
    test_bash_caps_long_line();
    T_REPORT();
}
