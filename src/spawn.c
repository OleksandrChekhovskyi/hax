/* SPDX-License-Identifier: MIT */
#include "spawn.h"

#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Parent-side signal mask. SIGINT and SIGQUIT match POSIX system()
 * semantics: while a foreground interactive child runs, terminal-
 * generated Ctrl-C / Ctrl-\ should drive the child, not the parent.
 * SIGPIPE matters when the parent writes into the child's stdin (the
 * pipe variant) — quitting the child early would otherwise terminate
 * hax via the default disposition. Saving/restoring locally keeps
 * the rest of the process unaffected. */
static void parent_ignore(struct sigaction *saved_int, struct sigaction *saved_quit,
                          struct sigaction *saved_pipe)
{
    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGINT, &ign, saved_int);
    sigaction(SIGQUIT, &ign, saved_quit);
    sigaction(SIGPIPE, &ign, saved_pipe);
}

static void parent_restore(const struct sigaction *saved_int, const struct sigaction *saved_quit,
                           const struct sigaction *saved_pipe)
{
    sigaction(SIGINT, saved_int, NULL);
    sigaction(SIGQUIT, saved_quit, NULL);
    sigaction(SIGPIPE, saved_pipe, NULL);
}

/* Runs in the post-fork child before execve. Reset the three signals
 * we ignored in the parent so the spawned program sees terminal
 * signals normally. signal() is in POSIX's async-signal-safe list and
 * the post-fork child is single-threaded — the simpler API is fine
 * here. */
static void child_default_signals(void)
{
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}

/* exec_into_shell: replace the calling process with `/bin/sh -c cmd`.
 * Returns only on failure (caller _exit's). */
static void exec_into_shell(const char *cmd)
{
    char *const argv[] = {(char *)"sh", (char *)"-c", (char *)cmd, NULL};
    execv("/bin/sh", argv);
}

/* Restart waitpid on EINTR. SIGINT/SIGQUIT/SIGPIPE are ignored above,
 * but SIGTERM/SIGHUP can still interrupt — those would have just
 * fired hax's own handler which calls _exit(), so we won't reach
 * here in that case. EINTR from anything else: retry. */
static int wait_child(pid_t pid)
{
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return status;
}

int spawn_run(const char *shell_cmd)
{
    if (!shell_cmd)
        return -1;
    struct sigaction si, sq, sp;
    parent_ignore(&si, &sq, &sp);

    pid_t pid = fork();
    if (pid < 0) {
        int saved_errno = errno;
        parent_restore(&si, &sq, &sp);
        errno = saved_errno;
        return -1;
    }
    if (pid == 0) {
        child_default_signals();
        exec_into_shell(shell_cmd);
        _exit(127);
    }
    int status = wait_child(pid);
    parent_restore(&si, &sq, &sp);
    return status;
}

int spawn_pipe_open(struct spawn_pipe *sp, const char *shell_cmd)
{
    if (!sp || !shell_cmd) {
        if (sp)
            memset(sp, 0, sizeof(*sp));
        errno = EINVAL;
        return -1;
    }
    memset(sp, 0, sizeof(*sp));

    int pfd[2];
    if (pipe(pfd) < 0)
        return -1;

    parent_ignore(&sp->saved_int, &sp->saved_quit, &sp->saved_pipe);

    pid_t pid = fork();
    if (pid < 0) {
        int saved_errno = errno;
        parent_restore(&sp->saved_int, &sp->saved_quit, &sp->saved_pipe);
        close(pfd[0]);
        close(pfd[1]);
        memset(sp, 0, sizeof(*sp));
        errno = saved_errno;
        return -1;
    }
    if (pid == 0) {
        child_default_signals();
        /* Wire the pipe's read end as stdin and drop both pipe fds
         * (after dup2 the read end has a second descriptor on fd 0;
         * the original needs to go too so the child sees a clean fd
         * table). */
        if (pfd[0] != STDIN_FILENO) {
            dup2(pfd[0], STDIN_FILENO);
            close(pfd[0]);
        }
        close(pfd[1]);
        exec_into_shell(shell_cmd);
        _exit(127);
    }
    /* Parent keeps only the write end. */
    close(pfd[0]);
    FILE *w = fdopen(pfd[1], "w");
    if (!w) {
        int saved_errno = errno;
        close(pfd[1]);
        /* Reap the child we just spawned so it doesn't linger as a
         * zombie. SIGTERM is needed because we can't rely on stdin EOF
         * to make the child exit — pagers like less open /dev/tty for
         * input rather than reading stdin, so closing the pipe alone
         * would leave wait_child blocking indefinitely. fdopen only
         * fails on OOM in practice, so this is a cold path. */
        kill(pid, SIGTERM);
        (void)wait_child(pid);
        parent_restore(&sp->saved_int, &sp->saved_quit, &sp->saved_pipe);
        memset(sp, 0, sizeof(*sp));
        errno = saved_errno;
        return -1;
    }
    sp->w = w;
    sp->pid = pid;
    return 0;
}

int spawn_pipe_close(struct spawn_pipe *sp)
{
    if (!sp || !sp->w)
        return 0;
    /* Close write end first so the child sees EOF and exits. */
    fclose(sp->w);
    sp->w = NULL;
    int status = wait_child(sp->pid);
    parent_restore(&sp->saved_int, &sp->saved_quit, &sp->saved_pipe);
    sp->pid = 0;
    return status;
}
