/* SPDX-License-Identifier: MIT */
#ifndef HAX_SPAWN_H
#define HAX_SPAWN_H

#include <signal.h>
#include <stdio.h>

/*
 * Process-spawn helpers for interactive shell-command children.
 *
 * Two patterns covered:
 *   - spawn_run: fork+exec a shell command and wait for it. Same role
 *     as POSIX system(), but additionally resets SIGPIPE in the child
 *     so common shell pipelines work the way the user expects.
 *   - spawn_pipe_open / spawn_pipe_close: fork+exec a shell command
 *     with its stdin attached to a write-mode FILE*. Drop-in for the
 *     "pipe content into a pager / filter" use case.
 *
 * Both ignore SIGINT, SIGQUIT, and SIGPIPE in the parent for the
 * duration of the child run (matching system()'s POSIX-mandated
 * etiquette plus SIGPIPE), and reset all three to SIG_DFL in the
 * child so the helper sees terminal signals normally — Ctrl-C in less
 * quits less, not hax; `:!yes | head` inside vim works the same as it
 * would from a regular shell.
 *
 * Out of scope: the bash tool's command runner. It owns a PTY, manages
 * a process group, enforces output caps and timeouts, and reads stdout
 * in a loop — too bespoke to share this code path. It does its own
 * signal reset post-fork.
 */

int spawn_run(const char *shell_cmd);

struct spawn_pipe {
    FILE *w;
    pid_t pid;
    /* Saved parent-side dispositions, restored by spawn_pipe_close. */
    struct sigaction saved_int;
    struct sigaction saved_quit;
    struct sigaction saved_pipe;
};

/* Opens a write-mode pipe to a shell command. On success, *sp is
 * populated and the caller writes content via sp->w then calls
 * spawn_pipe_close to flush, wait, and restore parent signals. On
 * failure, returns -1 with errno set; *sp is left zeroed and the
 * caller must NOT call spawn_pipe_close (no resources to release). */
int spawn_pipe_open(struct spawn_pipe *sp, const char *shell_cmd);

/* Closes the write side, waits for the child, restores parent
 * signals. Returns waitpid-style status, or -1 on internal error.
 * Idempotent on a zeroed struct (no-op + return 0) so callers can
 * pair it with a failed open without branching. */
int spawn_pipe_close(struct spawn_pipe *sp);

#endif /* HAX_SPAWN_H */
