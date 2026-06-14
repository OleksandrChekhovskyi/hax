/* SPDX-License-Identifier: MIT */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "harness.h"
#include "system/spawn.h"

/* Per-binary tmpdir so a parallel `meson test` run doesn't clobber
 * fixtures across processes. */
static char tmpdir[64];

static void tmp_setup(void)
{
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/haxspawn.%d", (int)getpid());
    mkdir(tmpdir, 0700);
}

static char *tmp_path(const char *name)
{
    static char buf[128];
    snprintf(buf, sizeof(buf), "%s/%s", tmpdir, name);
    return buf;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    fread(buf, 1, (size_t)n, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* ---------------- spawn_run ---------------- */

static void test_run_zero_exit(void)
{
    int rc = spawn_run("true");
    EXPECT(WIFEXITED(rc));
    EXPECT(WEXITSTATUS(rc) == 0);
}

static void test_run_nonzero_exit(void)
{
    int rc = spawn_run("exit 42");
    EXPECT(WIFEXITED(rc));
    EXPECT(WEXITSTATUS(rc) == 42);
}

static void test_run_executes_shell(void)
{
    char *path = tmp_path("ran.txt");
    char cmd[256];
    /* asprintf is a GNU/BSD extension, not in POSIX with the feature
     * test macros this project uses; snprintf into a stack buffer is
     * portable and fits since tmp_path output is bounded. */
    snprintf(cmd, sizeof(cmd), "echo hello > '%s'", path);
    int rc = spawn_run(cmd);
    EXPECT(WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
    char *content = slurp(path);
    EXPECT(content && strcmp(content, "hello\n") == 0);
    free(content);
    unlink(path);
}

/* The contract of spawn_run vs system() includes resetting SIGPIPE
 * in the child. We assert this directly: have the shell send itself
 * SIGPIPE. With SIG_DFL the shell dies via signal (WIFSIGNALED +
 * WTERMSIG == SIGPIPE); with SIG_IGN inherited from the parent it
 * would exit normally with 0. `trap -p` is unreliable here — bash
 * only reports traps set via the `trap` builtin, not inherited
 * dispositions, so a self-kill is the only portable signal. */
static void test_run_child_sigpipe_default(void)
{
    /* Reproduce hax's runtime parent state: ignore SIGPIPE here so
     * that without spawn_run's child reset, the child would inherit
     * SIG_IGN through fork+exec (SIG_IGN is preserved across exec). */
    struct sigaction ign, saved;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, &saved);

    int rc = spawn_run("kill -PIPE $$");

    sigaction(SIGPIPE, &saved, NULL);
    EXPECT(WIFSIGNALED(rc));
    EXPECT(WTERMSIG(rc) == SIGPIPE);
}

/* ---------------- spawn_pipe_open / close ---------------- */

static void test_pipe_writes_to_child_stdin(void)
{
    char *path = tmp_path("piped.txt");
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "cat > '%s'", path);
    struct spawn_pipe sp;
    EXPECT(spawn_pipe_open(&sp, cmd) == 0);
    fputs("hello from parent\n", sp.w);
    int rc = spawn_pipe_close(&sp);
    EXPECT(WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
    char *content = slurp(path);
    EXPECT(content && strcmp(content, "hello from parent\n") == 0);
    free(content);
    unlink(path);
}

static void test_pipe_close_after_failed_open_is_noop(void)
{
    /* A struct that was zeroed by a failed open must be safe to pass
     * to spawn_pipe_close. */
    struct spawn_pipe sp;
    memset(&sp, 0, sizeof(sp));
    EXPECT(spawn_pipe_close(&sp) == 0);
}

static void test_pipe_open_rejects_bad_args(void)
{
    EXPECT(spawn_pipe_open(NULL, "true") == -1);
    struct spawn_pipe sp;
    EXPECT(spawn_pipe_open(&sp, NULL) == -1);
}

/* SIGPIPE handling: a child that exits before reading anything would
 * cause the parent's fputs to fail with EPIPE. Without spawn_pipe's
 * SIG_IGN around the write window, that EPIPE would arrive as a
 * SIGPIPE and kill the test process. Verify we survive. */
static void test_pipe_early_child_exit_does_not_kill_parent(void)
{
    struct spawn_pipe sp;
    /* `true` ignores stdin and exits immediately. Give it a moment
     * to die before the parent writes — sleep would race; instead
     * we rely on the fact that even if we get there first, eventual
     * fputs after the child reaps will produce EPIPE silently. */
    EXPECT(spawn_pipe_open(&sp, "true") == 0);
    /* Push enough bytes that we're guaranteed to hit EPIPE at some
     * point if the child has already exited. PIPE_BUF on Linux/macOS
     * is at least 4096; one MiB easily exceeds that. */
    for (int i = 0; i < 4096; i++)
        fputs("xxxxxxxxxx", sp.w);
    int rc = spawn_pipe_close(&sp);
    /* Test process is still alive — that's the assertion. The
     * child's exit is also reapable. */
    EXPECT(WIFEXITED(rc));
}

/* ---------------- spawn_reap_if_exited ---------------- */

/* A pid that isn't our child reaps as "exited": waitpid returns
 * -1/ECHILD, which the helper treats as gone. Our own pid is a safe,
 * deterministic stand-in (never our child). */
static void test_reap_non_child_is_exited(void)
{
    EXPECT(spawn_reap_if_exited(getpid()) == 1);
}

/* A live child reads as not-yet-exited; we reap it ourselves after. */
static void test_reap_live_child(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        pause(); /* block until the parent kills us */
        _exit(0);
    }
    if (pid < 0) {
        EXPECT(0); /* fork failed: record and bail — never use pid (a
                    * -1 would make kill() signal the whole process group) */
        return;
    }
    EXPECT(spawn_reap_if_exited(pid) == 0);
    kill(pid, SIGKILL);
    (void)spawn_wait_child(pid);
}

/* An exited child gets reaped. Poll (bounded) because the child may
 * not have been scheduled to exit the instant we return from fork. */
static void test_reap_exited_child(void)
{
    pid_t pid = fork();
    if (pid == 0)
        _exit(0);
    if (pid < 0) {
        EXPECT(0); /* fork failed: bail before any waitpid(-1) */
        return;
    }
    int reaped = 0;
    for (int i = 0; i < 1000 && !reaped; i++) {
        if (spawn_reap_if_exited(pid)) {
            reaped = 1;
            break;
        }
        struct timespec ts = {0, 1000000}; /* 1ms */
        nanosleep(&ts, NULL);
    }
    EXPECT(reaped);
    /* Re-poll after the reap: waitpid now returns ECHILD, still
     * reported as exited — the idempotent shape keepawake's acquire
     * path relies on. */
    EXPECT(spawn_reap_if_exited(pid) == 1);
}

/* ---------------- spawn_child_redirect_null ---------------- */

/* After the redirect the child's stdin is /dev/null, which reads as
 * immediate EOF. The child reports the outcome through its exit status
 * so the parent can assert without shared memory. */
static void test_redirect_null_stdin_is_eof(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        spawn_child_redirect_null();
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        _exit(n == 0 ? 0 : 1);
    }
    if (pid < 0) {
        EXPECT(0); /* fork failed: bail before spawn_wait_child(-1) */
        return;
    }
    int rc = spawn_wait_child(pid);
    EXPECT(WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
}

/* ---------------- spawn_child_die_with_parent ---------------- */

/* With the parent still alive, the race-check must NOT self-exit — a
 * child passing its real parent pid should run through to its normal
 * exit. Guards against an inverted getppid() comparison. Run inside a
 * forked child so the (Linux) PR_SET_PDEATHSIG arm doesn't perturb the
 * test process; on non-Linux the call is a no-op and this still holds.
 * The death-propagation behavior itself (parent dies => SIGTERM) is
 * integration-level and racy (grandchild + reparenting), so it's left
 * out of this unit test. */
static void test_die_with_parent_alive_does_not_exit(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        spawn_child_die_with_parent(getppid());
        _exit(7);
    }
    if (pid < 0) {
        EXPECT(0); /* fork failed: bail before spawn_wait_child(-1) */
        return;
    }
    int rc = spawn_wait_child(pid);
    EXPECT(WIFEXITED(rc) && WEXITSTATUS(rc) == 7);
}

int main(void)
{
    tmp_setup();

    test_run_zero_exit();
    test_run_nonzero_exit();
    test_run_executes_shell();
    test_run_child_sigpipe_default();

    test_pipe_writes_to_child_stdin();
    test_pipe_close_after_failed_open_is_noop();
    test_pipe_open_rejects_bad_args();
    test_pipe_early_child_exit_does_not_kill_parent();

    test_reap_non_child_is_exited();
    test_reap_live_child();
    test_reap_exited_child();

    test_redirect_null_stdin_is_eof();

    test_die_with_parent_alive_does_not_exit();

    rmdir(tmpdir);
    T_REPORT();
}
