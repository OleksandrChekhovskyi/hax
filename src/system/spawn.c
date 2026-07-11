/* SPDX-License-Identifier: MIT */
#include "system/spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

/* Parent-side signal mask. SIGINT and SIGQUIT match POSIX system()
 * semantics: while a foreground interactive child runs, terminal-
 * generated Ctrl-C / Ctrl-\ should drive the child, not the parent.
 * SIGPIPE matters when the parent writes into the child's stdin (the
 * pipe variant) — quitting the child early would otherwise terminate
 * hax via the default disposition. Saving/restoring locally keeps
 * the rest of the process unaffected. */
void spawn_parent_ignore(struct sigaction *saved_int, struct sigaction *saved_quit,
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

void spawn_parent_restore(const struct sigaction *saved_int, const struct sigaction *saved_quit,
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
void spawn_child_default_signals(void)
{
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}

void spawn_child_redirect_null(void)
{
    int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0)
        return;
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO)
        close(devnull);
}

void spawn_child_die_with_parent(pid_t parent)
{
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    /* Close the fork/exec race: if the parent exited before the arm
     * above, the death signal will never come, so bail out now. */
    if (getppid() != parent)
        _exit(0);
#else
    (void)parent;
#endif
}

/* exec_into_shell: replace the calling process with `/bin/sh -c cmd`.
 * Returns only on failure (caller _exit's). */
static void exec_into_shell(const char *cmd)
{
    char *const argv[] = {(char *)"sh", (char *)"-c", (char *)cmd, NULL};
    execv("/bin/sh", argv);
}

/* Restart waitpid on EINTR. SIGINT/SIGQUIT/SIGPIPE are ignored by
 * spawn_parent_ignore, but SIGTERM/SIGHUP can still interrupt —
 * those would have just fired hax's own handler which calls _exit(),
 * so we won't reach here in that case. EINTR from anything else:
 * retry. */
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
    pid_t r = waitpid(pid, &status, WNOHANG);
    return r == pid || (r < 0 && errno == ECHILD);
}

int spawn_run(const char *shell_cmd)
{
    if (!shell_cmd)
        return -1;
    struct sigaction si, sq, sp;
    spawn_parent_ignore(&si, &sq, &sp);

    pid_t pid = fork();
    if (pid < 0) {
        int saved_errno = errno;
        spawn_parent_restore(&si, &sq, &sp);
        errno = saved_errno;
        return -1;
    }
    if (pid == 0) {
        spawn_child_default_signals();
        exec_into_shell(shell_cmd);
        _exit(127);
    }
    int status = spawn_wait_child(pid);
    spawn_parent_restore(&si, &sq, &sp);
    return status;
}

/* Shared body of spawn_pipe_open / spawn_pipe_open_read. The two
 * variants differ only in plumbing direction: which pipe end the child
 * dup2's onto which standard fd, and which end the parent keeps (fdopen
 * mode follows). Everything else — validation, parent signal etiquette,
 * fork/exec, and both error paths — is direction-agnostic. */
static int pipe_open_dir(struct spawn_pipe *sp, const char *shell_cmd, int read_mode)
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

    /* Write mode: child reads pfd[0] as stdin, parent writes pfd[1].
     * Read mode: child writes pfd[1] as stdout, parent reads pfd[0].
     * The child's other standard fds stay inherited so an interactive
     * child (pager, fzf) can use the terminal. */
    int child_fd = read_mode ? pfd[1] : pfd[0];
    int child_std = read_mode ? STDOUT_FILENO : STDIN_FILENO;
    int parent_fd = read_mode ? pfd[0] : pfd[1];

    spawn_parent_ignore(&sp->saved_int, &sp->saved_quit, &sp->saved_pipe);

    pid_t pid = fork();
    if (pid < 0) {
        int saved_errno = errno;
        spawn_parent_restore(&sp->saved_int, &sp->saved_quit, &sp->saved_pipe);
        close(pfd[0]);
        close(pfd[1]);
        memset(sp, 0, sizeof(*sp));
        errno = saved_errno;
        return -1;
    }
    if (pid == 0) {
        spawn_child_default_signals();
        /* Drop the parent's end first — the child never uses it, and
         * if the process started with a std fd closed, pipe() may have
         * assigned parent_fd == child_std, where closing it after the
         * dup2 below would re-close the freshly installed standard fd.
         * Then wire the child's end onto its standard fd, dropping the
         * original descriptor so the child sees a clean fd table. */
        close(parent_fd);
        if (child_fd != child_std) {
            dup2(child_fd, child_std);
            close(child_fd);
        }
        exec_into_shell(shell_cmd);
        _exit(127);
    }
    /* Parent keeps only its own end. */
    close(child_fd);
    FILE *f = fdopen(parent_fd, read_mode ? "r" : "w");
    if (!f) {
        int saved_errno = errno;
        close(parent_fd);
        /* Reap the child we just spawned so it doesn't linger as a
         * zombie. SIGTERM rather than relying on the closed pipe: an
         * interactive child (less, fzf) reads /dev/tty rather than the
         * pipe, so EOF/SIGPIPE alone wouldn't unblock it and
         * spawn_wait_child would hang. fdopen only fails on OOM in
         * practice, so this is a cold path. */
        kill(pid, SIGTERM);
        (void)spawn_wait_child(pid);
        spawn_parent_restore(&sp->saved_int, &sp->saved_quit, &sp->saved_pipe);
        memset(sp, 0, sizeof(*sp));
        errno = saved_errno;
        return -1;
    }
    if (read_mode)
        sp->r = f;
    else
        sp->w = f;
    sp->pid = pid;
    return 0;
}

int spawn_pipe_open(struct spawn_pipe *sp, const char *shell_cmd)
{
    return pipe_open_dir(sp, shell_cmd, 0);
}

int spawn_pipe_open_read(struct spawn_pipe *sp, const char *shell_cmd)
{
    return pipe_open_dir(sp, shell_cmd, 1);
}

int spawn_pipe_close(struct spawn_pipe *sp)
{
    if (!sp || (!sp->w && !sp->r))
        return 0;
    /* Close the parent's end first: for a write pipe the child sees
     * EOF and exits; for a read pipe a still-writing child gets
     * SIGPIPE (default disposition — see spawn_child_default_signals)
     * and dies, which is the desired teardown. */
    if (sp->w)
        fclose(sp->w);
    if (sp->r)
        fclose(sp->r);
    sp->w = NULL;
    sp->r = NULL;
    int status = spawn_wait_child(sp->pid);
    spawn_parent_restore(&sp->saved_int, &sp->saved_quit, &sp->saved_pipe);
    sp->pid = 0;
    return status;
}
