/* SPDX-License-Identifier: MIT */
#include "system/keepawake.h"

#include <signal.h>
#include <unistd.h>

#include "config.h"
#include "system/spawn.h"

#ifdef __APPLE__
#include <stdio.h>
#elif defined(__linux__)
#include <stdlib.h>
#endif

/* Calls occur on the main thread at the user-turn boundary. */
static pid_t helper_pid;

static void reap_dead_helper(void)
{
    if (helper_pid > 0 && spawn_reap_if_exited(helper_pid))
        helper_pid = 0;
}

#if defined(__APPLE__) || defined(__linux__)
static const char *resolve_executable(const char *const *candidates)
{
    for (size_t i = 0; candidates[i]; i++)
        if (access(candidates[i], X_OK) == 0)
            return candidates[i];
    return NULL;
}
#endif

#ifdef __linux__
static int systemd_inhibit_supports_no_ask_password(const char *path)
{
    static int supported = -1;
    if (supported >= 0)
        return supported;

    /* The option and interactive polkit agent were introduced together in systemd v257. Probe the
     * option instead of parsing a version so distro backports work too. */
    const char *const argv[] = {path, "--no-ask-password", "--version", NULL};
    size_t output_len;
    char *output = spawn_capture_stdout(argv, 4096, 1000, &output_len);
    supported = output != NULL;
    free(output);
    return supported;
}
#endif

static void spawn_helper(void)
{
#if defined(__APPLE__) || defined(__linux__)
    pid_t parent_pid = getpid();
    /* Resolve before fork; PATH lookup may allocate and deadlock after a multithreaded fork. */
#ifdef __APPLE__
    static const char *const candidates[] = {"/usr/bin/caffeinate", NULL};
    const char *helper_path = resolve_executable(candidates);
    if (!helper_path)
        return;

    char parent_pid_arg[16];
    snprintf(parent_pid_arg, sizeof(parent_pid_arg), "%d", (int)parent_pid);
    char *const argv[] = {(char *)"caffeinate", (char *)"-i", (char *)"-w", parent_pid_arg, NULL};
#elif defined(__linux__)
    static const char *const candidates[] = {"/usr/bin/systemd-inhibit", "/bin/systemd-inhibit",
                                             NULL};
    static const char *const sleep_candidates[] = {"/usr/bin/sleep", "/bin/sleep", NULL};
    const char *helper_path = resolve_executable(candidates);
    const char *sleep_path = resolve_executable(sleep_candidates);
    if (!helper_path || !sleep_path)
        return;

    /* systemd-inhibit uses execvp() for its command; keep PATH out of the trust boundary. */
    char *argv[9];
    size_t argc = 0;
    argv[argc++] = (char *)"systemd-inhibit";
    if (systemd_inhibit_supports_no_ask_password(helper_path))
        argv[argc++] = (char *)"--no-ask-password";
    argv[argc++] = (char *)"--what=idle";
    argv[argc++] = (char *)"--mode=block";
    argv[argc++] = (char *)"--who=hax";
    argv[argc++] = (char *)"--why=hax is running a turn";
    argv[argc++] = (char *)sleep_path;
    argv[argc++] = (char *)"2147483647"; /* INT32_MAX seconds; release ends it first. */
    argv[argc] = NULL;
#endif

    pid_t pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        /* Linux uses PDEATHSIG; caffeinate -w provides the same orphan cleanup on macOS. */
        spawn_child_die_with_parent(parent_pid, SIGTERM);
        spawn_child_reset_signals();
        spawn_child_redirect_stdio_to_null();
        execv(helper_path, argv);
        _exit(127);
    }
    helper_pid = pid;
#endif
}

void keepawake_acquire(void)
{
    if (!config_bool_or("keep_awake", 1))
        return;
    reap_dead_helper();
    if (helper_pid > 0)
        return;
    spawn_helper();
}

void keepawake_release(void)
{
    if (helper_pid <= 0)
        return;
    kill(helper_pid, SIGTERM);
    /* A helper that survives SIGTERM (caffeinate has been seen wedged on virtualized
     * macOS) must not hang the turn boundary; escalate instead of waiting forever. */
    (void)spawn_wait_child_timeout(helper_pid, 500);
    helper_pid = 0;
}
