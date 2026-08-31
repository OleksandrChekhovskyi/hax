/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "config.h"
#include "harness.h"
#include "xalloc.h"
#include "system/keepawake.h"

static void expect_no_children(void)
{
    int status;
    errno = 0;
    pid_t child = waitpid(-1, &status, WNOHANG);
    EXPECT(child == -1 && errno == ECHILD);
}

static void test_release_without_acquire(void)
{
    keepawake_release();
    expect_no_children();
}

static void test_acquire_release_cycle(void)
{
    keepawake_acquire();
    keepawake_release();
    expect_no_children();
}

static void test_double_acquire(void)
{
    keepawake_acquire();
    keepawake_acquire();
    keepawake_release();
    expect_no_children();
}

static void test_disabled_is_noop(void)
{
    config_set_override("keep_awake", "0");
    keepawake_acquire();
    expect_no_children();
    keepawake_release();
    config_set_override("keep_awake", "1");
}

static void test_sleep_not_resolved_via_path(void)
{
    char *dir = t_tempdir();
    char *fake_sleep = xasprintf("%s/sleep", dir);
    char *marker = xasprintf("%s/ran", dir);

    FILE *file = fopen(fake_sleep, "w");
    EXPECT(file != NULL);
    if (!file)
        goto out;
    fprintf(file, "#!/bin/sh\ntouch '%s'\n", marker);
    fclose(file);
    chmod(fake_sleep, 0755);

    const char *current_path = getenv("PATH");
    char *saved_path = current_path ? xstrdup(current_path) : NULL;
    char *test_path = xasprintf("%s:%s", dir, saved_path ? saved_path : "");
    setenv("PATH", test_path, 1);
    free(test_path);

    keepawake_acquire();
    int fake_ran = 0;
    for (int i = 0; i < 50 && !fake_ran; i++) {
        fake_ran = access(marker, F_OK) == 0;
        if (!fake_ran) {
            struct timespec delay = {0, 10 * 1000000};
            nanosleep(&delay, NULL);
        }
    }
    keepawake_release();

    EXPECT(!fake_ran);

    if (saved_path) {
        setenv("PATH", saved_path, 1);
        free(saved_path);
    } else {
        unsetenv("PATH");
    }

out:
    free(marker);
    free(fake_sleep);
}

int main(void)
{
    config_set_override("keep_awake", "1");
    test_release_without_acquire();
    test_acquire_release_cycle();
    test_double_acquire();
    test_disabled_is_noop();
    test_sleep_not_resolved_via_path();
    T_REPORT();
}
