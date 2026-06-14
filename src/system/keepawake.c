/* SPDX-License-Identifier: MIT */
#include "system/keepawake.h"

#include "config.h"
#include "system/spawn.h"

#include <signal.h>
#include <unistd.h>

#ifdef __APPLE__
#include <stdio.h> /* snprintf, for the caffeinate -w <pid> argument */
#endif

/* The single helper holding the assertion, or 0 when none is up. The
 * agent is single-threaded around the user-turn seam that drives this,
 * so a plain global needs no locking. */
static pid_t helper_pid = 0;

/* Clear helper_pid if the helper already exited on its own (so a later
 * acquire respawns rather than assuming a live assertion). */
static void reap_if_dead(void)
{
    if (helper_pid > 0 && spawn_reap_if_exited(helper_pid))
        helper_pid = 0;
}

/* First executable path in `candidates` (NULL-terminated), or NULL when
 * none exists. Called in the parent so the post-fork child can execv an
 * already-resolved absolute path instead of a PATH search (see
 * spawn_helper). */
#if defined(__APPLE__) || defined(__linux__)
static const char *resolve_helper(const char *const *candidates)
{
    for (size_t i = 0; candidates[i]; i++)
        if (access(candidates[i], X_OK) == 0)
            return candidates[i];
    return NULL;
}
#endif

/* fork+exec the platform helper. Sets helper_pid on success; a fork or
 * exec failure leaves it 0 (the exec failure surfaces as the child's
 * _exit(127), reaped by the next acquire's reap_if_dead). No-op on
 * unsupported platforms, or when the helper binary isn't installed. */
static void spawn_helper(void)
{
    pid_t parent = getpid();
#ifdef __APPLE__
    /* -i: inhibit idle *system* sleep only (display may still blank).
     * -w: exit once hax (parent) exits — self-heals an orphan. */
    char pidbuf[16];
    snprintf(pidbuf, sizeof(pidbuf), "%d", (int)parent);
    char *const argv[] = {(char *)"caffeinate", (char *)"-i", (char *)"-w", pidbuf, NULL};
    static const char *const candidates[] = {"/usr/bin/caffeinate", NULL};
#elif defined(__linux__)
    static const char *const candidates[] = {"/usr/bin/systemd-inhibit", "/bin/systemd-inhibit",
                                             NULL};
    /* systemd-inhibit execvp()s its wrapped command, so resolve sleep to
     * an absolute path here too: a bare "sleep" would go through the
     * caller's PATH and run automatically every turn (keep_awake is on by
     * default), letting `PATH=/tmp/evil:$PATH hax` hijack it — and would
     * also break silently when PATH lacks coreutils. An absolute path
     * makes systemd-inhibit's execvp skip the PATH search. NULL (no sleep
     * binary) skips the spawn. */
    static const char *const sleep_candidates[] = {"/usr/bin/sleep", "/bin/sleep", NULL};
    const char *sleep_path = resolve_helper(sleep_candidates);
    if (!sleep_path)
        return;
    /* sleep for ~68 years (i32::MAX seconds, accepted by coreutils
     * sleep); the inhibitor lock lives as long as systemd-inhibit, which
     * we terminate on release or which dies with us via PDEATHSIG. */
    char *const argv[] = {(char *)"systemd-inhibit",
                          (char *)"--what=idle",
                          (char *)"--mode=block",
                          (char *)"--who=hax",
                          (char *)"--why=hax is running a turn",
                          (char *)sleep_path,
                          (char *)"2147483647",
                          NULL};
#else
    (void)parent;
    return;
#endif

#if defined(__APPLE__) || defined(__linux__)
    /* Resolve to an absolute path here in the (multithreaded) parent: the
     * post-fork child must touch only async-signal-safe calls before exec,
     * and execvp's PATH search can malloc — deadlocking on the arena lock
     * if a vanished thread (the interrupt watcher, a provider probe) held
     * it at fork time. Same rule the bash runner follows. */
    const char *path = resolve_helper(candidates);
    if (!path)
        return; /* helper not installed — silent no-op */

    pid_t pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        /* Die with hax so a SIGKILLed parent (which skips release)
         * doesn't strand the lock. Linux-only; macOS self-heals via
         * caffeinate's -w above. */
        spawn_child_die_with_parent(parent);
        spawn_child_default_signals();
        spawn_child_redirect_null();
        execv(path, argv); /* absolute path — no async-unsafe PATH search */
        _exit(127);
    }
    helper_pid = pid;
#endif
}

void keepawake_acquire(void)
{
    if (!config_bool_or("keep_awake", 1))
        return;
    reap_if_dead();
    if (helper_pid > 0)
        return; /* already holding the assertion */
    spawn_helper();
}

void keepawake_release(void)
{
    if (helper_pid <= 0)
        return;
    kill(helper_pid, SIGTERM);
    (void)spawn_wait_child(helper_pid);
    helper_pid = 0;
}
