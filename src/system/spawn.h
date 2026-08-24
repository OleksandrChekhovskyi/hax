/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_SPAWN_H
#define HAX_SYSTEM_SPAWN_H

#include <signal.h>
#include <stdio.h>

#ifndef _WIN32
struct spawn_signal_state {
    struct sigaction sigint;
    struct sigaction sigquit;
    struct sigaction sigpipe;
};
#endif

/* Run `shell_cmd` via /bin/sh and return its waitpid status, or -1 on error. */
int spawn_shell_wait(const char *shell_cmd);

/* Prepare a trusted `sh -c` command for a child that must decode what hax renders, by supplying an
 * LC_CTYPE the environment does not. Takes ownership and returns an allocated command, or the
 * original where the environment already suffices. Not for tool commands, whose locale is the
 * user's to pin. */
char *spawn_shell_cmd_force_utf8(char *shell_cmd);

/* Run `argv` directly, with stdin and stderr redirected to /dev/null, and capture stdout.
 * Return malloc'd output of 1..max_bytes on a zero exit status, or NULL on error, timeout,
 * overflow, or empty output. `argv`, argv[0], and out_len must be non-NULL and timeout_ms must be
 * positive. The child is killed and reaped on timeout or overflow. On success, *out_len receives
 * the output size. */
char *spawn_capture_stdout(const char *const *argv, size_t max_bytes, int timeout_ms,
                           size_t *out_len);

#ifndef _WIN32
/* Ignore terminal signals in the parent while a child runs. The child must reset them before
 * exec, and the parent must restore `state` after waiting. This follows system() semantics for
 * SIGINT and SIGQUIT; SIGPIPE also protects parent writes to a child that exits early. */
void spawn_parent_ignore_signals(struct spawn_signal_state *state);
void spawn_parent_restore_signals(const struct spawn_signal_state *state);
void spawn_child_reset_signals(void);
void spawn_child_redirect_stdio_to_null(void);
void spawn_child_die_with_parent(pid_t parent_pid, int signal_number);

/* POSIX-only helpers for platform integrations that fork directly. */
int spawn_wait_child(pid_t pid);
int spawn_wait_child_timeout(pid_t pid, int timeout_ms);
int spawn_reap_if_exited(pid_t pid);
#endif

struct spawn_process;

enum spawn_end {
    SPAWN_END_EXITED,
    SPAWN_END_SIGNALED,
    SPAWN_END_FORCED,
};

struct spawn_status {
    enum spawn_end end;
    int code; /* exit code for EXITED, signal number for SIGNALED, otherwise zero */
};

/* Start an isolated Bash process with merged output. `shell` and `argv0` are required on POSIX and
 * ignored on Windows, where the validated Git Bash installation is used. The returned opaque
 * process owns the process tree until spawn_process_wait consumes it. */
struct spawn_process *spawn_process_start_bash(const char *shell, const char *argv0,
                                               const char *command, char *const envp[],
                                               int *output_fd);
void spawn_process_terminate(struct spawn_process *process, int signal_number);
int spawn_process_exit_seen(struct spawn_process *process, int *exit_seen);
int spawn_process_wait(struct spawn_process *process, struct spawn_status *status);

#ifdef _WIN32
int spawn_win32_bash_available(void);
char *spawn_win32_bash_path(void);
#endif

struct spawn_pipe {
    FILE *stream;
#ifdef _WIN32
    struct spawn_process *process;
#else
    pid_t pid;
    struct spawn_signal_state parent_signals;
#endif
};

/* Open a pipe to a shell command. The write variant connects `stream` to the child's stdin; the
 * read variant connects it to the child's stdout. Other standard descriptors remain inherited.
 * On failure, return -1 with errno set and leave `pipe` zeroed. */
int spawn_pipe_open_write(struct spawn_pipe *pipe, const char *shell_cmd);
int spawn_pipe_open_read(struct spawn_pipe *pipe, const char *shell_cmd);

/* Close the stream, wait for the child, restore parent signals, and return waitpid status. A NULL
 * or zeroed pipe is a successful no-op. */
int spawn_pipe_close(struct spawn_pipe *pipe);

#endif /* HAX_SYSTEM_SPAWN_H */
