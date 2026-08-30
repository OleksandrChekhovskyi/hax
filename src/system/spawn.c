/* SPDX-License-Identifier: MIT */
#include "system/spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "buf.h"
#include "util.h"

/* Everything a pinned LC_ALL was covering besides the charset. Clearing it is what lets LC_CTYPE
 * apply at all, so each category is restated at the value it had: hax is entitled to the encoding
 * its own text is in, not to the user's collation, messages or number format. */
static const char *const PINNED_CATEGORIES[] = {
    "LC_COLLATE", "LC_MONETARY", "LC_NUMERIC", "LC_TIME", "LC_MESSAGES",
};

char *spawn_shell_cmd_force_utf8(char *shell_cmd)
{
    const char *ctype = shell_cmd ? locale_child_ctype_override() : NULL;
    if (!ctype)
        return shell_cmd;

    struct buf command;
    buf_init(&command);
    buf_append_str(&command, "export");

    const char *pinned = getenv("LC_ALL");
    char *quoted_pinned = pinned && *pinned ? shell_single_quote(pinned) : NULL;
    for (size_t i = 0; quoted_pinned && i < sizeof(PINNED_CATEGORIES) / sizeof(*PINNED_CATEGORIES);
         i++) {
        char *assignment = xasprintf(" %s=%s", PINNED_CATEGORIES[i], quoted_pinned);
        buf_append_str(&command, assignment);
        free(assignment);
    }
    free(quoted_pinned);

    /* An assignment prefix would only reach a simple command, and `sh` rejects one before the
     * `{ ...; }` a caller may well pass. Exporting first reaches any command shape. setlocale()
     * ignores an empty LC_ALL, which the pinned one would otherwise outrank. */
    char *quoted_ctype = shell_single_quote(ctype);
    char *tail = xasprintf(" LC_ALL= LC_CTYPE=%s; %s", quoted_ctype, shell_cmd);
    buf_append_str(&command, tail);
    free(tail);
    free(quoted_ctype);
    free(shell_cmd);
    return buf_steal(&command);
}

void spawn_parent_ignore_signals(struct spawn_signal_state *state)
{
    struct sigaction ignored;
    memset(&ignored, 0, sizeof(ignored));
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    sigaction(SIGINT, &ignored, &state->sigint);
    sigaction(SIGQUIT, &ignored, &state->sigquit);
    sigaction(SIGPIPE, &ignored, &state->sigpipe);
}

void spawn_parent_restore_signals(const struct spawn_signal_state *state)
{
    sigaction(SIGINT, &state->sigint, NULL);
    sigaction(SIGQUIT, &state->sigquit, NULL);
    sigaction(SIGPIPE, &state->sigpipe, NULL);
}

void spawn_child_reset_signals(void)
{
    /* signal() is async-signal-safe and avoids constructing sigaction state after fork. */
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}

void spawn_child_redirect_stdio_to_null(void)
{
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0)
        return;
    dup2(null_fd, STDIN_FILENO);
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
    if (null_fd > STDERR_FILENO)
        close(null_fd);
}

void spawn_child_die_with_parent(pid_t parent_pid, int signal_number)
{
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, signal_number);
    /* PR_SET_PDEATHSIG does not report a parent death that precedes the prctl call. */
    if (getppid() != parent_pid)
        _exit(0);
#else
    (void)parent_pid;
    (void)signal_number;
#endif
}

int spawn_wait_child(pid_t pid)
{
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return status;
}

int spawn_reap_if_exited(pid_t pid)
{
    int status;
    pid_t waited_pid = waitpid(pid, &status, WNOHANG);
    return waited_pid == pid || (waited_pid < 0 && errno == ECHILD);
}

static void exec_shell(const char *shell_cmd)
{
    char *const argv[] = {(char *)"sh", (char *)"-c", (char *)shell_cmd, NULL};
    execv("/bin/sh", argv);
}

int spawn_shell_wait(const char *shell_cmd)
{
    if (!shell_cmd) {
        errno = EINVAL;
        return -1;
    }

    struct spawn_signal_state signals;
    spawn_parent_ignore_signals(&signals);

    pid_t pid = fork();
    if (pid < 0) {
        int saved_errno = errno;
        spawn_parent_restore_signals(&signals);
        errno = saved_errno;
        return -1;
    }
    if (pid == 0) {
        spawn_child_reset_signals();
        exec_shell(shell_cmd);
        _exit(127);
    }

    int status = spawn_wait_child(pid);
    spawn_parent_restore_signals(&signals);
    return status;
}

int spawn_detached(const char *const *argv)
{
    struct spawn_signal_state signals;
    spawn_parent_ignore_signals(&signals);
    pid_t pid = fork();
    if (pid == 0) {
        /* Double-fork: the grandchild is reparented to init, so a child that stays alive
         * indefinitely is never waited on, killed, or left as a zombie. */
        pid_t grandchild = fork();
        if (grandchild != 0)
            _exit(grandchild < 0 ? 127 : 0);
        spawn_child_redirect_stdio_to_null();
        spawn_child_reset_signals();
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status = pid < 0 ? -1 : spawn_wait_child(pid);
    spawn_parent_restore_signals(&signals);
    return status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

enum spawn_pipe_mode {
    SPAWN_PIPE_READ,
    SPAWN_PIPE_WRITE,
};

static int spawn_pipe_open_mode(struct spawn_pipe *result, const char *shell_cmd,
                                enum spawn_pipe_mode mode)
{
    if (!result || !shell_cmd) {
        if (result)
            memset(result, 0, sizeof(*result));
        errno = EINVAL;
        return -1;
    }
    memset(result, 0, sizeof(*result));

    int pipe_fds[2];
    if (pipe(pipe_fds) < 0)
        return -1;

    int child_fd = mode == SPAWN_PIPE_READ ? pipe_fds[1] : pipe_fds[0];
    int child_target = mode == SPAWN_PIPE_READ ? STDOUT_FILENO : STDIN_FILENO;
    int parent_fd = mode == SPAWN_PIPE_READ ? pipe_fds[0] : pipe_fds[1];

    spawn_parent_ignore_signals(&result->parent_signals);

    pid_t pid = fork();
    if (pid < 0) {
        int saved_errno = errno;
        spawn_parent_restore_signals(&result->parent_signals);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        memset(result, 0, sizeof(*result));
        errno = saved_errno;
        return -1;
    }
    if (pid == 0) {
        spawn_child_reset_signals();
        /* Closing this first handles a parent fd that reused the target standard descriptor. */
        close(parent_fd);
        if (child_fd != child_target) {
            if (dup2(child_fd, child_target) < 0)
                _exit(127);
            close(child_fd);
        }
        exec_shell(shell_cmd);
        _exit(127);
    }

    close(child_fd);
    FILE *stream = fdopen(parent_fd, mode == SPAWN_PIPE_READ ? "r" : "w");
    if (!stream) {
        int saved_errno = errno;
        close(parent_fd);
        /* A child reading /dev/tty may not notice that its pipe end closed. */
        kill(pid, SIGTERM);
        (void)spawn_wait_child(pid);
        spawn_parent_restore_signals(&result->parent_signals);
        memset(result, 0, sizeof(*result));
        errno = saved_errno;
        return -1;
    }

    result->stream = stream;
    result->pid = pid;
    return 0;
}

int spawn_pipe_open_write(struct spawn_pipe *pipe, const char *shell_cmd)
{
    return spawn_pipe_open_mode(pipe, shell_cmd, SPAWN_PIPE_WRITE);
}

int spawn_pipe_open_read(struct spawn_pipe *pipe, const char *shell_cmd)
{
    return spawn_pipe_open_mode(pipe, shell_cmd, SPAWN_PIPE_READ);
}

int spawn_pipe_close(struct spawn_pipe *pipe)
{
    if (!pipe || !pipe->stream)
        return 0;

    /* Closing first delivers EOF to a reader or SIGPIPE to a writer before waitpid. */
    fclose(pipe->stream);
    pipe->stream = NULL;
    int status = spawn_wait_child(pipe->pid);
    spawn_parent_restore_signals(&pipe->parent_signals);
    pipe->pid = 0;
    return status;
}

/* EOF does not imply child exit, so retain the capture deadline while reaping. */
static int wait_child_until(pid_t pid, long deadline_ms)
{
    const struct timespec poll_interval = {.tv_nsec = 5 * 1000000};

    for (;;) {
        int status;
        pid_t waited_pid = waitpid(pid, &status, WNOHANG);
        if (waited_pid == pid)
            return status;
        if (waited_pid < 0 && errno != EINTR)
            return -1;
        if (monotonic_ms() >= deadline_ms) {
            kill(pid, SIGKILL);
            return spawn_wait_child(pid);
        }
        nanosleep(&poll_interval, NULL);
    }
}

int spawn_wait_child_timeout(pid_t pid, int timeout_ms)
{
    return wait_child_until(pid, monotonic_ms() + timeout_ms);
}

char *spawn_capture_stdout(const char *const *argv, size_t max_bytes, int timeout_ms,
                           size_t *out_len)
{
    if (!argv || !argv[0] || !out_len || timeout_ms <= 0) {
        errno = EINVAL;
        return NULL;
    }

    int pipe_fds[2];
    if (pipe(pipe_fds) < 0)
        return NULL;

    struct spawn_signal_state signals;
    spawn_parent_ignore_signals(&signals);

    pid_t pid = fork();
    if (pid < 0) {
        int saved_errno = errno;
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        spawn_parent_restore_signals(&signals);
        errno = saved_errno;
        return NULL;
    }
    if (pid == 0) {
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        if (pipe_fds[1] != STDOUT_FILENO)
            close(pipe_fds[1]);

        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        spawn_child_reset_signals();
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(pipe_fds[1]);

    long deadline_ms = monotonic_ms() + timeout_ms;
    struct buf output;
    buf_init(&output);
    int capture_failed = 0;
    char chunk[65536];
    for (;;) {
        long remaining_ms = deadline_ms - monotonic_ms();
        if (remaining_ms <= 0) {
            capture_failed = 1;
            break;
        }

        struct pollfd poll_fd = {.fd = pipe_fds[0], .events = POLLIN};
        int poll_result = poll(&poll_fd, 1, remaining_ms > INT_MAX ? INT_MAX : (int)remaining_ms);
        if (poll_result < 0 && errno == EINTR)
            continue;
        if (poll_result <= 0) {
            capture_failed = 1;
            break;
        }

        ssize_t bytes_read = read(pipe_fds[0], chunk, sizeof(chunk));
        if (bytes_read < 0 && errno == EINTR)
            continue;
        if (bytes_read < 0) {
            capture_failed = 1;
            break;
        }
        if (bytes_read == 0)
            break;
        if ((size_t)bytes_read > max_bytes - output.len) {
            capture_failed = 1;
            break;
        }
        buf_append(&output, chunk, (size_t)bytes_read);
    }
    close(pipe_fds[0]);

    int status;
    if (capture_failed) {
        kill(pid, SIGKILL);
        status = spawn_wait_child(pid);
    } else {
        status = wait_child_until(pid, deadline_ms);
    }
    spawn_parent_restore_signals(&signals);

    if (capture_failed || status < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        output.len == 0) {
        buf_free(&output);
        return NULL;
    }
    *out_len = output.len;
    return buf_steal(&output);
}
