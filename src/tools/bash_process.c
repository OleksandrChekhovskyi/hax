/* SPDX-License-Identifier: MIT */
#include "tools/bash_process.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "config.h"
#include "util.h"
#include "terminal/interrupt.h"
#include "tools/bash_env.h"
#include "tools/bash_output.h"
#include "tools/bash_shell.h"

struct shell_process {
    pid_t pid;
    int output_fd;
};

static long deadline_after(long now_ms, long duration_ms)
{
    return duration_ms > LONG_MAX - now_ms ? LONG_MAX : now_ms + duration_ms;
}

/* The child creates its process group after fork. Keep `pid` unreaped while using the fallback. */
static void signal_process_tree(pid_t pid, int signal_number)
{
    if (kill(-pid, signal_number) < 0 && errno == ESRCH)
        kill(pid, signal_number);
}

/* This runs after fork in a multithreaded process; use only async-signal-safe calls. */
static void exec_shell_child(const char *shell, const char *argv0, const char *command,
                             char *const envp[])
{
    close(STDIN_FILENO);
    (void)open("/dev/null", O_RDONLY);
    char *const argv[] = {(char *)argv0, (char *)"-c", (char *)command, NULL};
    execve(shell, argv, envp);
    _exit(127);
}

static char *start_shell(const char *command, struct shell_process *process)
{
    /* Resolve everything before fork so the child can avoid allocator and environment locks. */
    char **envp = bash_build_child_env();
    char *shell = bash_resolve_shell();
    const char *argv0 = strrchr(shell, '/');
    argv0 = argv0 ? argv0 + 1 : shell;

    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) {
        char *error = xasprintf("pipe: %s", strerror(errno));
        free(shell);
        free(envp);
        return error;
    }

    pid_t pid = fork();
    if (pid < 0) {
        char *error = xasprintf("fork: %s", strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        free(shell);
        free(envp);
        return error;
    }
    if (pid == 0) {
        close(pipe_fds[0]);
        /* A separate session isolates descendants and removes access to the agent's terminal. */
        setsid();
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        if (pipe_fds[1] > STDERR_FILENO)
            close(pipe_fds[1]);
        exec_shell_child(shell, argv0, command, envp);
    }

    close(pipe_fds[1]);
    free(shell);
    free(envp);
    process->pid = pid;
    process->output_fd = pipe_fds[0];
    return NULL;
}

/* Return the grace deadline, or 0 when the process tree was killed immediately. */
static long start_shutdown(pid_t pid, long now_ms, long grace_ms)
{
    if (grace_ms <= 0) {
        signal_process_tree(pid, SIGKILL);
        return 0;
    }
    signal_process_tree(pid, SIGTERM);
    return deadline_after(now_ms, grace_ms);
}

static int shell_has_exited(pid_t pid, int *exited)
{
    siginfo_t info = {0};
    int result = waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT);
    if (result == 0 && info.si_pid == pid)
        *exited = 1;
    return result;
}

static int poll_timeout_ms(long deadline)
{
    const int default_poll_ms = 10;
    if (deadline <= 0)
        return default_poll_ms;

    long remaining_ms = deadline - monotonic_ms();
    if (remaining_ms <= 0)
        return 0;
    return remaining_ms < default_poll_ms ? (int)remaining_ms : default_poll_ms;
}

static void display_suffix(tool_display_fn display, void *display_data, size_t total_bytes,
                           int binary, int displayed_body, enum bash_stop_reason reason,
                           long timeout_ms, int wait_status)
{
    char *suffix = bash_output_format_suffix(total_bytes, binary, displayed_body, reason,
                                             timeout_ms, wait_status);

    /* A newline aborts any unterminated escape sequence before the binary marker's '['. */
    if (binary && displayed_body)
        display("\n", 1, display_data);
    if (*suffix)
        display(suffix, strlen(suffix), display_data);
    free(suffix);
}

char *bash_run_command(const char *command, long timeout_ms, tool_display_fn display,
                       void *display_data)
{
    struct shell_process process = {0};
    char *error = start_shell(command, &process);
    if (error)
        return error;

    long deadline = timeout_ms > 0 ? deadline_after(monotonic_ms(), timeout_ms) : 0;
    long grace_ms = config_duration_ms("bash.timeout_grace");
    long grace_deadline = 0;
    enum bash_stop_reason stop_reason = BASH_STOP_NONE;
    int shell_exited = 0;
    int wait_status = 0;
    int binary = 0;
    int displayed_body = 0;
    struct bash_output *output = bash_output_create(output_cap_bytes());
    char chunk[4096];

    for (;;) {
        long now_ms = monotonic_ms();

        /* User interruption wins if it coincides with the timeout. */
        if (stop_reason == BASH_STOP_NONE && interrupt_requested()) {
            stop_reason = BASH_STOP_INTERRUPT;
            if (shell_exited)
                break;
            grace_deadline = start_shutdown(process.pid, now_ms, grace_ms);
            if (grace_deadline == 0)
                break;
        }
        if (stop_reason == BASH_STOP_NONE && deadline > 0 && now_ms >= deadline) {
            stop_reason = BASH_STOP_TIMEOUT;
            if (shell_exited)
                break;
            grace_deadline = start_shutdown(process.pid, now_ms, grace_ms);
            if (grace_deadline == 0)
                break;
        }
        if (stop_reason != BASH_STOP_NONE && grace_deadline > 0 && now_ms >= grace_deadline) {
            signal_process_tree(process.pid, SIGKILL);
            break;
        }

        /* WNOWAIT keeps the pid reserved until all process-tree signaling is finished. */
        if (!shell_exited) {
            int status_result = shell_has_exited(process.pid, &shell_exited);
            if (shell_exited && stop_reason == BASH_STOP_NONE)
                signal_process_tree(process.pid, SIGKILL);
            else if (status_result < 0 && errno != EINTR)
                break;
        }

        long active_deadline = stop_reason == BASH_STOP_NONE ? deadline : grace_deadline;
        struct pollfd poll_fd = {.fd = process.output_fd, .events = POLLIN};
        int poll_result = poll(&poll_fd, 1, poll_timeout_ms(active_deadline));
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (poll_result == 0)
            continue;

        ssize_t bytes_read = read(process.output_fd, chunk, sizeof(chunk));
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (bytes_read == 0) {
            /* EOF cannot yield more cleanup output, so no grace period remains useful. */
            signal_process_tree(process.pid, SIGKILL);
            break;
        }

        if (!binary && memchr(chunk, '\0', (size_t)bytes_read))
            binary = 1;
        if (display && !binary) {
            display(chunk, (size_t)bytes_read, display_data);
            displayed_body = 1;
        }
        bash_output_append(output, chunk, (size_t)bytes_read);
        if (bash_output_size(output) >= (size_t)BASH_OUTPUT_DRAIN_LIMIT) {
            signal_process_tree(process.pid, SIGKILL);
            break;
        }
    }
    close(process.output_fd);

    while (waitpid(process.pid, &wait_status, 0) < 0) {
        if (errno != EINTR)
            break;
    }

    if (display) {
        display_suffix(display, display_data, bash_output_size(output), binary, displayed_body,
                       stop_reason, timeout_ms, wait_status);
    }
    char *result = bash_output_finish(output, binary, stop_reason, timeout_ms, wait_status);
    bash_output_destroy(output);
    return result;
}
