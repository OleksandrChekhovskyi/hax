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

/* Lower-level signal/wait helpers for callers that own their own
 * fork+exec lifecycle (e.g. argv-based exec with custom child fd
 * wiring, where neither spawn_run nor spawn_pipe_open's shell-and-
 * FILE* shape fits). Exposed so we can have one canonical
 * implementation of the POSIX system()-style parent/child signal
 * etiquette plus the EINTR-tolerant waitpid loop, instead of
 * duplicating ~10 lines per caller. */

/* Save current SIGINT/SIGQUIT/SIGPIPE dispositions and replace them
 * with SIG_IGN. Caller must pair with spawn_parent_restore. The
 * three-signal set matches POSIX system()'s etiquette plus SIGPIPE,
 * so a child writing to a closed pipe — or a terminal-generated
 * Ctrl-C / Ctrl-\ during the child run — doesn't take down hax. */
void spawn_parent_ignore(struct sigaction *saved_int, struct sigaction *saved_quit,
                         struct sigaction *saved_pipe);

/* Restore dispositions saved by spawn_parent_ignore. */
void spawn_parent_restore(const struct sigaction *saved_int, const struct sigaction *saved_quit,
                          const struct sigaction *saved_pipe);

/* Reset SIGINT/SIGQUIT/SIGPIPE to SIG_DFL — call from the post-fork
 * child before exec so the spawned program sees terminal signals
 * normally (Ctrl-C in less quits less, not the parent). */
void spawn_child_default_signals(void);

/* waitpid(pid, &status, 0) with an EINTR retry loop. Returns the
 * waitpid status word on success, -1 on non-EINTR error. */
int spawn_wait_child(pid_t pid);

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
