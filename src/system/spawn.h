/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_SPAWN_H
#define HAX_SYSTEM_SPAWN_H

#include <signal.h>
#include <stdio.h>

struct spawn_signal_state {
    struct sigaction sigint;
    struct sigaction sigquit;
    struct sigaction sigpipe;
};

/* Run `shell_cmd` via /bin/sh and return its waitpid status, or -1 on error. */
int spawn_shell_wait(const char *shell_cmd);

/* Run `argv` directly, with stdin and stderr redirected to /dev/null, and capture stdout.
 * Return malloc'd output of 1..max_bytes on a zero exit status, or NULL on error, timeout,
 * overflow, or empty output. `argv`, argv[0], and out_len must be non-NULL and timeout_ms must be
 * positive. The child is killed and reaped on timeout or overflow. On success, *out_len receives
 * the output size. */
char *spawn_capture_stdout(const char *const *argv, size_t max_bytes, int timeout_ms,
                           size_t *out_len);

/* Ignore terminal signals in the parent while a child runs. The child must reset them before
 * exec, and the parent must restore `state` after waiting. This follows system() semantics for
 * SIGINT and SIGQUIT; SIGPIPE also protects parent writes to a child that exits early. */
void spawn_parent_ignore_signals(struct spawn_signal_state *state);
void spawn_parent_restore_signals(const struct spawn_signal_state *state);
void spawn_child_reset_signals(void);

/* Redirect all three standard descriptors to /dev/null on a best-effort basis. */
void spawn_child_redirect_stdio_to_null(void);

/* On Linux, arrange for the post-fork child to receive `signal_number` when `parent_pid` dies.
 * `parent_pid` must be captured before fork so the helper can close the fork/arm race. This is a
 * no-op where PR_SET_PDEATHSIG is unavailable. */
void spawn_child_die_with_parent(pid_t parent_pid, int signal_number);

/* Return the child's waitpid status, retrying EINTR, or -1 on error. */
int spawn_wait_child(pid_t pid);

/* Reap an exited child and return 1; return 0 if it is running or waitpid otherwise fails.
 * ECHILD counts as exited. */
int spawn_reap_if_exited(pid_t pid);

struct spawn_pipe {
    FILE *stream;
    pid_t pid;
    struct spawn_signal_state parent_signals;
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
