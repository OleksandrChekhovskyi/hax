/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "config.h"
#include "harness.h"
#include "system/keepawake.h"
#include "util.h"

/* On Linux the helper is `systemd-inhibit`, which isn't present in many
 * CI/sandbox environments; on macOS it's `caffeinate`. Either way the
 * contract under test is the same and doesn't depend on the assertion
 * actually taking effect: acquire/release must be safe, idempotent, and
 * leave no child behind. We can't peek at the static helper_pid, so we
 * assert the observable behavior — no crash, and no unreaped child
 * lingering after release (waitpid would otherwise find one). */

/* Assert the process has no outstanding children. waitpid over the whole
 * process group returns -1/ECHILD only when nothing is reapable; a
 * returned pid (an exited-but-unreaped helper) or 0 (a still-running one)
 * both mean a helper leaked, so accept neither. */
static void expect_no_children(void)
{
    int status;
    errno = 0;
    pid_t r = waitpid(-1, &status, WNOHANG);
    EXPECT(r == -1 && errno == ECHILD);
}

/* release with nothing acquired is a no-op. */
static void test_release_without_acquire(void)
{
    keepawake_release();
    EXPECT(1); /* reached here without crashing */
}

/* A full acquire/release cycle returns to a clean slate: after release
 * there must be no reapable child of ours. */
static void test_acquire_release_cycle(void)
{
    keepawake_acquire();
    keepawake_release();
    expect_no_children();
}

/* acquire is idempotent — a second call while already holding must not
 * spawn a second helper or leak the first. After a single release the
 * slate is clean. */
static void test_double_acquire(void)
{
    keepawake_acquire();
    keepawake_acquire();
    keepawake_release();
    expect_no_children();
}

/* When the feature is disabled via config, acquire spawns nothing and
 * release stays a no-op. */
static void test_disabled_is_noop(void)
{
    config_set_override("keep_awake", "0");
    keepawake_acquire();
    expect_no_children();
    keepawake_release();
    config_set_override("keep_awake", "1");
}

/* A malicious `sleep` placed first on PATH must never run. systemd-inhibit
 * execvp()s its wrapped command, but keepawake hands it an absolute sleep
 * path, so there is no PATH entry to hijack. On hosts without a working
 * systemd-inhibit (or non-Linux, where the wrapped-command shape doesn't
 * exist at all) acquire runs no PATH-resolved command, so the check
 * trivially holds — it bites only where the inhibitor actually executes. */
static void test_sleep_not_resolved_via_path(void)
{
    config_set_override("keep_awake", "1");

    char dir[64], fake[96], marker[96];
    snprintf(dir, sizeof(dir), "/tmp/haxkeepawake.%d", (int)getpid());
    mkdir(dir, 0700);
    snprintf(fake, sizeof(fake), "%s/sleep", dir);
    snprintf(marker, sizeof(marker), "%s/ran", dir);

    /* Fake `sleep`: drop a marker if it ever runs, then exit. */
    FILE *f = fopen(fake, "w");
    EXPECT(f != NULL);
    if (!f) {
        rmdir(dir);
        return;
    }
    fprintf(f, "#!/bin/sh\ntouch '%s'\n", marker);
    fclose(f);
    chmod(fake, 0755);

    const char *cur = getenv("PATH");
    char *saved = cur ? xstrdup(cur) : NULL;
    char newpath[4096];
    snprintf(newpath, sizeof(newpath), "%s:%s", dir, saved ? saved : "");
    setenv("PATH", newpath, 1);

    keepawake_acquire();
    /* Give a wrongly-PATH-resolved sleep time to be exec'd (bounded; bail
     * the instant the marker appears). The correct path never touches it,
     * so the loop runs to completion — ~500ms is plenty for the buggy
     * exec to manifest while keeping the suite fast. */
    int ran = 0;
    for (int i = 0; i < 50 && !ran; i++) {
        if (access(marker, F_OK) == 0) {
            ran = 1;
            break;
        }
        struct timespec ts = {0, 10000000}; /* 10ms */
        nanosleep(&ts, NULL);
    }
    keepawake_release();

    EXPECT(!ran); /* the PATH sleep must never have executed */

    if (saved) {
        setenv("PATH", saved, 1);
        free(saved);
    } else {
        unsetenv("PATH");
    }
    unlink(fake);
    unlink(marker);
    rmdir(dir);
}

int main(void)
{
    test_release_without_acquire();
    test_acquire_release_cycle();
    test_double_acquire();
    test_disabled_is_noop();
    test_sleep_not_resolved_via_path();
    T_REPORT();
}
