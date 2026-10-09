/* SPDX-License-Identifier: MIT */
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "config.h"
#include "harness.h"
#include "output.h"
#include "xalloc.h"
#include "commands/tasks.h"
#include "system/clock.h"
#include "text/completion.h"
#include "tools/bash_fixtures.h"
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

/* Adopt a sleeping child as the running task `name`, listed as `command`; task_registry_shutdown
 * kills it. The child closes its output at once, so the drainer finishes instead of polling the
 * pipe until shutdown joins it. A refused adoption leaves the child and its descriptors with the
 * caller, so they are released here. */
static void adopt_sleeping_task(const char *name, const char *command)
{
    int pipe_fds[2];
    EXPECT(pipe(pipe_fds) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        execlp("sleep", "sleep", "30", (char *)NULL);
        _exit(127);
    }
    EXPECT(pid > 0);
    close(pipe_fds[1]);

    char *spool_path = xasprintf("%s/spool", t_tempdir());
    int spool_fd = open(spool_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    EXPECT(spool_fd >= 0);
    if (!task_adopt(pid, pipe_fds[0], command, name, monotonic_ms(), spool_fd, spool_path, 0, 0,
                    0)) {
        FAIL("task '%s' was not adopted", name);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(pipe_fds[0]);
        close(spool_fd);
        free(spool_path);
    }
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
    adopt_sleeping_task("build", "sleep 30");
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

static void run_command(void *argument)
{
    tasks_command(*(const char *const *)argument);
}

/* Return what /tasks printed for `argument` (NULL for none), without styling. */
static char *command_output(const char *argument)
{
    char *raw = t_capture_stdout(run_command, &argument);
    char *out = t_strip_sgr(raw);
    free(raw);
    return out;
}

static void expect_output_has(const char *argument, const char *expected)
{
    char *out = command_output(argument);
    if (!strstr(out, expected))
        FAIL("/tasks %s printed '%s', expected '%s'", argument ? argument : "", out, expected);
    free(out);
}

static void test_command_reports_empty_and_disabled(void)
{
    expect_output_has(NULL, "no background tasks");

    config_set_override("no_tasks", "on");
    expect_output_has(NULL, "background tasks are disabled (no_tasks)");
    expect_output_has("kill all", "background tasks are disabled (no_tasks)");
    config_set_override("no_tasks", NULL);
}

static void test_command_lists_tasks_within_width(void)
{
    adopt_sleeping_task("build", "sleep 30");
    adopt_sleeping_task("t1", "sleep 30 # a long comment that cannot fit a narrow display row");

    setenv("HAX_DISPLAY_WIDTH", "100", 1);
    char *out = command_output(NULL);
    EXPECT(strstr(out, "  build  running · ") != NULL);
    EXPECT(strstr(out, "  t1     running · ") != NULL);
    EXPECT(strstr(out, "a long comment that cannot fit a narrow display row") != NULL);
    free(out);

    setenv("HAX_DISPLAY_WIDTH", "40", 1);
    out = command_output(NULL);
    unsetenv("HAX_DISPLAY_WIDTH");
    t_expect_rows_fit(out, 40);
    EXPECT(strstr(out, "build") != NULL && strstr(out, "t1") != NULL);
    free(out);
    task_registry_shutdown();
}

static void test_command_kills_named_or_all_tasks(void)
{
    adopt_sleeping_task("build", "sleep 30");
    adopt_sleeping_task("lint", "sleep 30");
    adopt_sleeping_task("docs", "sleep 30");

    expect_output_has("kill nope", "stopped 0 tasks");
    expect_output_has("kill build", "stopped 1 task\n");
    expect_output_has("kill  all", "stopped 2 tasks");
    task_registry_shutdown();
}

static void test_command_rejects_other_arguments(void)
{
    expect_output_has("kill", "usage: /tasks kill <id>... | kill all");
    expect_output_has("killall", "usage: /tasks [kill <id>... | kill all]");
    expect_output_has("stop t1", "usage: /tasks [kill <id>... | kill all]");
}

int main(void)
{
    setenv("HAX_BASH_TIMEOUT_GRACE", TEST_KILL_GRACE, 1);

    test_choices_follow_kill_grammar();
    test_choices_offer_running_tasks();
    test_choices_off_without_tasks();
    test_command_reports_empty_and_disabled();
    test_command_lists_tasks_within_width();
    test_command_kills_named_or_all_tasks();
    test_command_rejects_other_arguments();
    T_REPORT();
}
