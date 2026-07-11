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
 * Out of scope: the bash tool's command runner. It owns the child's
 * stdout/stderr pipe, manages a process group, enforces output caps
 * and timeouts, and reads in a loop — too bespoke to share this code
 * path. It does its own signal reset post-fork.
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

/* Redirect the post-fork child's stdin/stdout/stderr to /dev/null
 * before exec — for background helpers that should neither read the
 * terminal nor scribble on the REPL (cf. spawn_run / spawn_pipe_open,
 * which deliberately inherit parent stdio for interactive children).
 * Best-effort: if /dev/null can't be opened the child just keeps the
 * inherited descriptors. */
void spawn_child_redirect_null(void);

/* Arrange for the post-fork child to die with its parent: on Linux,
 * request SIGTERM via PR_SET_PDEATHSIG so a parent that exits without
 * reaping (a SIGKILL, say) doesn't strand the child. `parent` is the
 * pid captured *before* fork; this re-checks getppid() against it and
 * self-exits if the parent already died in the fork/exec window (the
 * death signal only fires for deaths after the arm). A no-op where
 * PR_SET_PDEATHSIG is unavailable; callers that also need a
 * non-Linux backstop (e.g. a self-terminating exec) supply that
 * separately. */
void spawn_child_die_with_parent(pid_t parent);

/* waitpid(pid, &status, 0) with an EINTR retry loop. Returns the
 * waitpid status word on success, -1 on non-EINTR error. */
int spawn_wait_child(pid_t pid);

/* Non-blocking reap: if `pid` has already exited, reap it and return 1;
 * return 0 while it is still running. ECHILD (not our child, or already
 * reaped elsewhere) counts as exited; any other waitpid error returns 0
 * so a transient failure doesn't falsely report a live child as gone.
 * The WNOHANG sibling of spawn_wait_child, for callers polling a
 * long-lived background helper they may later respawn. */
int spawn_reap_if_exited(pid_t pid);

struct spawn_pipe {
    FILE *w; /* write-mode variant (spawn_pipe_open); NULL otherwise */
    FILE *r; /* read-mode variant (spawn_pipe_open_read); NULL otherwise */
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

/* Read-mode sibling of spawn_pipe_open: the child's stdout is the pipe
 * and the caller reads its output via sp->r. stdin/stderr stay
 * inherited, so an interactive child (fzf) can still reach the tty.
 * Same contract otherwise. Unlike popen(), the child gets the full
 * spawn signal etiquette — SIGINT/SIGQUIT/SIGPIPE reset to SIG_DFL
 * (popen's child would inherit the parent's SIG_IGN across exec,
 * leaving terminal Ctrl-C unable to drive it). */
int spawn_pipe_open_read(struct spawn_pipe *sp, const char *shell_cmd);

/* Closes the parent's pipe end, waits for the child, restores parent
 * signals. Returns waitpid-style status, or -1 on internal error.
 * Idempotent on a zeroed struct (no-op + return 0) so callers can
 * pair it with a failed open without branching. For a read pipe,
 * closing before the child finishes writing tears it down via
 * SIGPIPE — at default disposition in the child, per the above. */
int spawn_pipe_close(struct spawn_pipe *sp);

#endif /* HAX_SPAWN_H */
