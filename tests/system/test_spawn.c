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
#include "system/spawn.h"

static const char *tmpdir;

static const char *tmp_path(const char *name)
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
    char *content = slurp(path);
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
    char *content = slurp(path);
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
    EXPECT(WIFSIGNALED(status));
    EXPECT(WTERMSIG(status) == SIGINT);
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

int main(void)
{
    tmpdir = t_tempdir();

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
    test_reap_live_child();
    test_reap_exited_child();

    test_redirect_null_stdin_is_eof();

    test_die_with_parent_alive_does_not_exit();

    T_REPORT();
}
