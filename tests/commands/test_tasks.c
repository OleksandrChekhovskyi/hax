/* SPDX-License-Identifier: MIT */
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "config.h"
#include "harness.h"
#include "xalloc.h"
#include "commands/tasks.h"
#include "system/clock.h"
#include "text/completion.h"
#include "tools/task_registry.h"

static void expect_choices(const char *preceding, const char *const *expected)
{
    struct completion choices = {0};
    tasks_choices(preceding, &choices);

    size_t expected_count = 0;
    while (expected[expected_count])
        expected_count++;
    EXPECT(choices.count == expected_count);
    for (size_t i = 0; i < choices.count && i < expected_count; i++)
        EXPECT_STR_EQ(choices.candidates[i], expected[i]);
    completion_free(&choices);
}

/* Adopt a sleeping child as the running task `name`; task_registry_shutdown kills it. */
static void adopt_sleeping_task(const char *name)
{
    int pipe_fds[2];
    EXPECT(pipe(pipe_fds) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        close(pipe_fds[0]);
        execlp("sleep", "sleep", "30", (char *)NULL);
        _exit(127);
    }
    EXPECT(pid > 0);
    close(pipe_fds[1]);

    char *spool_path = xasprintf("%s/spool", t_tempdir());
    int spool_fd = open(spool_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    EXPECT(spool_fd >= 0);
    EXPECT(task_adopt(pid, pipe_fds[0], "sleep 30", name, monotonic_ms(), spool_fd, spool_path, 0,
                      0, 0) != NULL);
}

static void test_choices_follow_kill_grammar(void)
{
    expect_choices("", (const char *const[]){"kill", NULL});
    expect_choices("kill", (const char *const[]){"all", NULL});
    expect_choices("kill all", (const char *const[]){NULL});
    expect_choices("all", (const char *const[]){NULL});
}

static void test_choices_offer_running_tasks(void)
{
    adopt_sleeping_task("build");
    expect_choices("kill", (const char *const[]){"all", "build", NULL});
    expect_choices("kill build", (const char *const[]){NULL});
    task_registry_shutdown();
}

static void test_choices_off_without_tasks(void)
{
    config_set_override("no_tasks", "on");
    expect_choices("", (const char *const[]){NULL});
    config_set_override("no_tasks", NULL);
}

int main(void)
{
    /* Choice assertions assume tasks are enabled; the variable leaks in from any hax parent or
     * user environment. */
    unsetenv("HAX_NO_TASKS");

    test_choices_follow_kill_grammar();
    test_choices_offer_running_tasks();
    test_choices_off_without_tasks();
    T_REPORT();
}
