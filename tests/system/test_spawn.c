/* SPDX-License-Identifier: MIT */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
/* The wait macros are provided by <sys/wait.h> per POSIX; glibc also leaks
 * them through <stdlib.h>, so the include cleaner cannot attribute them. */
#include <sys/wait.h> // IWYU pragma: keep

#include "harness.h"
#include "xalloc.h"
#include "system/locale.h"
#include "system/spawn.h"

static const char *tmpdir;

static const char *tmp_path(const char *name)
{
    static char buf[128];
    snprintf(buf, sizeof(buf), "%s/%s", tmpdir, name);
    return buf;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    EXPECT(fseek(f, 0, SEEK_END) == 0);
    long file_size = ftell(f);
    EXPECT(file_size >= 0);
    EXPECT(fseek(f, 0, SEEK_SET) == 0);
    char *content = malloc((size_t)file_size + 1);
    size_t got = fread(content, 1, (size_t)file_size, f);
    content[got] = '\0';
    fclose(f);
    return content;
}

static void test_shell_zero_exit(void)
{
    int status = spawn_shell_wait("true");
    EXPECT(WIFEXITED(status));
    EXPECT(WEXITSTATUS(status) == 0);
}

static void test_shell_nonzero_exit(void)
{
    int status = spawn_shell_wait("exit 42");
    EXPECT(WIFEXITED(status));
    EXPECT(WEXITSTATUS(status) == 42);
}

static void test_shell_executes_command(void)
{
    const char *path = tmp_path("ran.txt");
    char shell_cmd[256];
    snprintf(shell_cmd, sizeof(shell_cmd), "echo hello > '%s'", path);
    int status = spawn_shell_wait(shell_cmd);
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char *content = read_file(path);
    EXPECT(content && strcmp(content, "hello\n") == 0);
    free(content);
}

static void test_shell_child_sigpipe_default(void)
{
    /* Ignored dispositions survive exec, so the child must explicitly reset SIGPIPE. */
    struct sigaction ignored, saved;
    memset(&ignored, 0, sizeof(ignored));
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    sigaction(SIGPIPE, &ignored, &saved);

    int status = spawn_shell_wait("kill -PIPE $$");

    sigaction(SIGPIPE, &saved, NULL);
    EXPECT(WIFSIGNALED(status));
    EXPECT(WTERMSIG(status) == SIGPIPE);
}

static void test_pipe_writes_to_child_stdin(void)
{
    const char *path = tmp_path("piped.txt");
    char shell_cmd[256];
    snprintf(shell_cmd, sizeof(shell_cmd), "cat > '%s'", path);
    struct spawn_pipe pipe;
    EXPECT(spawn_pipe_open_write(&pipe, shell_cmd) == 0);
    fputs("hello from parent\n", pipe.stream);
    int status = spawn_pipe_close(&pipe);
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char *content = read_file(path);
    EXPECT(content && strcmp(content, "hello from parent\n") == 0);
    free(content);
}

static void test_pipe_close_after_failed_open_is_noop(void)
{
    struct spawn_pipe pipe;
    EXPECT(spawn_pipe_open_write(&pipe, NULL) == -1);
    EXPECT(spawn_pipe_close(&pipe) == 0);
}

static void test_pipe_write_rejects_bad_args(void)
{
    EXPECT(spawn_pipe_open_write(NULL, "true") == -1);
    struct spawn_pipe pipe;
    EXPECT(spawn_pipe_open_write(&pipe, NULL) == -1);
}

static void test_pipe_early_child_exit_does_not_kill_parent(void)
{
    struct spawn_pipe pipe;
    EXPECT(spawn_pipe_open_write(&pipe, "true") == 0);
    for (int i = 0; i < 4096; i++)
        fputs("xxxxxxxxxx", pipe.stream);
    int status = spawn_pipe_close(&pipe);
    EXPECT(WIFEXITED(status));
}

static void test_pipe_read_child_stdout(void)
{
    struct spawn_pipe pipe;
    EXPECT(spawn_pipe_open_read(&pipe, "printf 'picked/path.c\\n'") == 0);
    char line[64];
    EXPECT(fgets(line, sizeof(line), pipe.stream) != NULL);
    EXPECT(strcmp(line, "picked/path.c\n") == 0);
    int status = spawn_pipe_close(&pipe);
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_pipe_read_nonzero_exit(void)
{
    struct spawn_pipe pipe;
    EXPECT(spawn_pipe_open_read(&pipe, "exit 42") == 0);
    char line[8];
    EXPECT(fgets(line, sizeof(line), pipe.stream) == NULL);
    int status = spawn_pipe_close(&pipe);
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 42);
}

static void test_pipe_read_rejects_bad_args(void)
{
    EXPECT(spawn_pipe_open_read(NULL, "true") == -1);
    struct spawn_pipe pipe;
    EXPECT(spawn_pipe_open_read(&pipe, NULL) == -1);
    EXPECT(spawn_pipe_close(&pipe) == 0);
}

static void test_pipe_read_child_sigint_default(void)
{
    struct spawn_pipe pipe;
    EXPECT(spawn_pipe_open_read(&pipe, "kill -INT $$; exit 0") == 0);
    int status = spawn_pipe_close(&pipe);
    /* What matters is that the child did not inherit SIG_IGN: reaching the trailing `exit 0` would
     * mean the signal was ignored. Which of the other two outcomes occurs is the shell's choice —
     * OpenBSD's ksh reports 128+SIGINT where dash and bash re-raise it. */
    EXPECT((WIFSIGNALED(status) && WTERMSIG(status) == SIGINT) ||
           (WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGINT));
}

static void test_pipe_read_early_close_kills_writer(void)
{
    struct spawn_pipe pipe;
    EXPECT(spawn_pipe_open_read(&pipe, "while :; do echo x; done") == 0);
    char line[8];
    EXPECT(fgets(line, sizeof(line), pipe.stream) != NULL);
    int status = spawn_pipe_close(&pipe);
    EXPECT(WIFSIGNALED(status));
    EXPECT(WTERMSIG(status) == SIGPIPE);
}

/* Closing stdout forces a pipe fd to reuse the child's target descriptor. */
static void test_pipe_read_survives_closed_stdout(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        close(STDOUT_FILENO);
        struct spawn_pipe pipe;
        if (spawn_pipe_open_read(&pipe, "echo hi") != 0)
            _exit(2);
        char line[8];
        int output_matches = fgets(line, sizeof(line), pipe.stream) && strcmp(line, "hi\n") == 0;
        int status = spawn_pipe_close(&pipe);
        _exit((output_matches && WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1);
    }
    if (pid < 0) {
        EXPECT(0); /* fork failed: bail before spawn_wait_child(-1) */
        return;
    }
    int status = spawn_wait_child(pid);
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_capture_collects_stdout(void)
{
    const char *argv[] = {"printf", "hello", NULL};
    size_t output_len = 0;
    char *output = spawn_capture_stdout(argv, 5, 5000, &output_len);
    EXPECT(output != NULL && output_len == 5 && memcmp(output, "hello", 5) == 0);
    free(output);
}

static void test_capture_nonzero_exit_is_null(void)
{
    const char *argv[] = {"sh", "-c", "echo x; exit 1", NULL};
    size_t output_len = 0;
    EXPECT(spawn_capture_stdout(argv, 1024, 5000, &output_len) == NULL);
}

static void test_capture_empty_output_is_null(void)
{
    const char *argv[] = {"true", NULL};
    size_t output_len = 0;
    EXPECT(spawn_capture_stdout(argv, 1024, 5000, &output_len) == NULL);
}

static void test_capture_missing_helper_is_null(void)
{
    const char *argv[] = {"hax-no-such-helper", NULL};
    size_t output_len = 0;
    EXPECT(spawn_capture_stdout(argv, 1024, 5000, &output_len) == NULL);
}

static void test_capture_overflow_is_null(void)
{
    const char *argv[] = {"printf", "0123456789", NULL};
    size_t output_len = 0;
    EXPECT(spawn_capture_stdout(argv, 4, 5000, &output_len) == NULL);
}

static void test_capture_rejects_bad_args(void)
{
    const char *const empty_argv[] = {NULL};
    const char *const argv[] = {"printf", "hello", NULL};
    size_t output_len;

    EXPECT(spawn_capture_stdout(NULL, 1024, 5000, &output_len) == NULL);
    EXPECT(spawn_capture_stdout(empty_argv, 1024, 5000, &output_len) == NULL);
    EXPECT(spawn_capture_stdout(argv, 1024, 0, &output_len) == NULL);
    EXPECT(spawn_capture_stdout(argv, 1024, 5000, NULL) == NULL);
}

static long elapsed_ms(const struct timespec *started_at)
{
    struct timespec finished_at;
    clock_gettime(CLOCK_MONOTONIC, &finished_at);
    return (finished_at.tv_sec - started_at->tv_sec) * 1000 +
           (finished_at.tv_nsec - started_at->tv_nsec) / 1000000;
}

static void test_capture_timeout_kills_stalled_helper(void)
{
    const char *argv[] = {"sleep", "30", NULL};
    struct timespec started_at;
    clock_gettime(CLOCK_MONOTONIC, &started_at);
    size_t output_len = 0;
    EXPECT(spawn_capture_stdout(argv, 1024, 200, &output_len) == NULL);
    EXPECT(elapsed_ms(&started_at) < 10000);
}

/* EOF on stdout does not prove the child exited. */
static void test_capture_eof_then_hang_is_bounded(void)
{
    const char *argv[] = {"sh", "-c", "echo hi; exec 1>&-; sleep 30", NULL};
    struct timespec started_at;
    clock_gettime(CLOCK_MONOTONIC, &started_at);
    size_t output_len = 0;
    EXPECT(spawn_capture_stdout(argv, 1024, 200, &output_len) == NULL);
    EXPECT(elapsed_ms(&started_at) < 10000);
}

static void test_reap_non_child_is_exited(void)
{
    EXPECT(spawn_reap_if_exited(getpid()) == 1);
}

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

/* The detached grandchild runs and is not this process's child, so only its side effect is
 * observable — poll (bounded) for a file it publishes by rename, so a partial write is never
 * read. */
static void test_detached_runs_helper(void)
{
    const char *out_path = tmp_path("detached.txt");
    char *command =
        xasprintf("printf detached > '%s.tmp' && mv '%s.tmp' '%s'", out_path, out_path, out_path);
    const char *argv[] = {"/bin/sh", "-c", command, NULL};
    EXPECT(spawn_detached(argv) == 0);
    free(command);

    char *content = NULL;
    for (int i = 0; i < 300 && !content; i++) {
        content = read_file(out_path);
        if (!content) {
            struct timespec pause_ts = {.tv_nsec = 10 * 1000 * 1000};
            nanosleep(&pause_ts, NULL);
        }
    }
    EXPECT(content != NULL);
    if (content)
        EXPECT_STR_EQ(content, "detached");
    free(content);
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
        const struct timespec retry_interval = {.tv_nsec = 1000000};
        nanosleep(&retry_interval, NULL);
    }
    EXPECT(reaped);
    EXPECT(spawn_reap_if_exited(pid) == 1);
}

static void test_redirect_null_stdin_is_eof(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        spawn_child_redirect_stdio_to_null();
        char c;
        ssize_t bytes_read = read(STDIN_FILENO, &c, 1);
        _exit(bytes_read == 0 ? 0 : 1);
    }
    if (pid < 0) {
        EXPECT(0); /* fork failed: bail before spawn_wait_child(-1) */
        return;
    }
    int status = spawn_wait_child(pid);
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* Isolate the Linux parent-death signal from the test runner. */
static void test_die_with_parent_alive_does_not_exit(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        spawn_child_die_with_parent(getppid(), SIGTERM);
        _exit(7);
    }
    if (pid < 0) {
        EXPECT(0); /* fork failed: bail before spawn_wait_child(-1) */
        return;
    }
    int status = spawn_wait_child(pid);
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 7);
}

static void test_force_utf8_leaves_a_capable_environment_alone(void)
{
    unsetenv("LC_ALL");
    setenv("LC_CTYPE", "C", 1);
    locale_init_utf8();
    if (!locale_have_utf8())
        T_SKIP("no UTF-8 locale to switch to");

    char *cmd = spawn_shell_cmd_force_utf8(xstrdup("less -R"));
    EXPECT_STR_EQ(cmd, "less -R");
    free(cmd);
}

/* A pinned LC_ALL outranks the published LC_CTYPE, leaving the command itself to carry one. */
static void test_force_utf8_overrides_a_pinned_lc_all(void)
{
    setenv("LC_ALL", "C", 1);
    locale_init_utf8();
    if (!locale_have_utf8())
        T_SKIP("no UTF-8 locale to switch to");

    char *cmd = spawn_shell_cmd_force_utf8(xstrdup("less -R"));
    EXPECT(strstr(cmd, "LC_CTYPE=") != NULL);
    EXPECT(strstr(cmd, " less -R") != NULL);
    free(cmd);
}

/* Reads the locale back out of a real child, the string it was spelled into saying nothing about
 * whether a shell accepts it. What the child reports is its environment rather than its charset,
 * because locale(1) is not POSIX and busybox omits it. */
static char *force_utf8_child_locale(const char *command)
{
    char *cmd = spawn_shell_cmd_force_utf8(xstrdup(command));
    struct spawn_pipe pipe;
    if (spawn_pipe_open_read(&pipe, cmd) != 0) {
        free(cmd);
        return NULL;
    }
    char line[128] = {0};
    char *reported = fgets(line, sizeof(line), pipe.stream) ? xstrdup(line) : NULL;
    spawn_pipe_close(&pipe);
    free(cmd);
    if (reported)
        reported[strcspn(reported, "\n")] = '\0';
    return reported;
}

/* Emptying LC_ALL is what lets LC_CTYPE apply, so the child should report the charset, no LC_ALL,
 * and a collation still at the pinned value rather than one re-derived from LANG. */
static void expect_child_locale(const char *command)
{
    const char *override = locale_child_ctype_override();
    if (!override)
        T_SKIP("no UTF-8 locale to switch to");
    char *want = xasprintf("%s||C", override);

    char *got = force_utf8_child_locale(command);
    EXPECT(got != NULL);
    if (got)
        EXPECT_STR_EQ(got, want);
    free(got);
    free(want);
}

#define LOCALE_PROBE "printf '%s|%s|%s\\n' \"$LC_CTYPE\" \"$LC_ALL\" \"$LC_COLLATE\""

static void test_force_utf8_reaches_the_child(void)
{
    setenv("LANG", "de_DE.UTF-8", 1);
    setenv("LC_ALL", "C", 1);
    locale_init_utf8();

    expect_child_locale(LOCALE_PROBE);
}

/* The file picker passes a brace group, and `sh` rejects an assignment prefix before one — so the
 * override has to survive command shapes other than a simple command. */
static void test_force_utf8_accepts_a_compound_command(void)
{
    setenv("LANG", "de_DE.UTF-8", 1);
    setenv("LC_ALL", "C", 1);
    locale_init_utf8();

    expect_child_locale("{ " LOCALE_PROBE "; } | cat");
}

int main(void)
{
    tmpdir = t_tempdir();

    test_force_utf8_leaves_a_capable_environment_alone();
    test_force_utf8_overrides_a_pinned_lc_all();
    test_force_utf8_reaches_the_child();
    test_force_utf8_accepts_a_compound_command();

    test_shell_zero_exit();
    test_shell_nonzero_exit();
    test_shell_executes_command();
    test_shell_child_sigpipe_default();

    test_pipe_writes_to_child_stdin();
    test_pipe_close_after_failed_open_is_noop();
    test_pipe_write_rejects_bad_args();
    test_pipe_early_child_exit_does_not_kill_parent();

    test_pipe_read_child_stdout();
    test_pipe_read_nonzero_exit();
    test_pipe_read_rejects_bad_args();
    test_pipe_read_child_sigint_default();
    test_pipe_read_early_close_kills_writer();
    test_pipe_read_survives_closed_stdout();

    test_capture_collects_stdout();
    test_capture_nonzero_exit_is_null();
    test_capture_empty_output_is_null();
    test_capture_missing_helper_is_null();
    test_capture_overflow_is_null();
    test_capture_rejects_bad_args();
    test_capture_timeout_kills_stalled_helper();
    test_capture_eof_then_hang_is_bounded();

    test_reap_non_child_is_exited();
    test_detached_runs_helper();

    test_reap_live_child();
    test_reap_exited_child();

    test_redirect_null_stdin_is_eof();

    test_die_with_parent_alive_does_not_exit();

    T_REPORT();
}
