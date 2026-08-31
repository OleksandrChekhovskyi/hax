/* SPDX-License-Identifier: MIT */
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "agent_core.h"
#include "buf.h"
#include "harness.h"
#include "provider.h"
#include "tool.h"
#include "xalloc.h"
#include "system/fs.h"
#include "tools/bash_process.h"
#include "tools/task_helpers.h"
#include "tools/task_registry.h"

static void test_background_fast_command_returns_sync(void)
{
    char *out = call_bash_background("echo hi");
    EXPECT_STR_EQ(out, "hi\n\n[finished during launch; no task created]");
    free(out);

    /* The requested name is reported dead so the model does not wait on it, and stays free.
     * The note is a model-only tail: the display ends with the command's own output. */
    const char *footer = "\n[finished during launch; task quick not created]";
    for (int round = 0; round < 2; round++) {
        struct display_capture capture = {0};
        buf_init(&capture.buf);
        struct tool_run_ctx ctx = {.display = append_display, .display_data = &capture};
        out =
            TOOL_BASH.run("{\"command\":\"echo hi\",\"background\":true,\"name\":\"quick\"}", &ctx);
        size_t out_len = strlen(out), footer_len = strlen(footer);
        EXPECT(out_len > footer_len && strcmp(out + out_len - footer_len, footer) == 0);
        EXPECT(ctx.output_hidden_tail == footer_len);
        free(out);
        EXPECT(capture.buf.data != NULL);
        EXPECT_STR_EQ(capture.buf.data, "hi\n");
        buf_free(&capture.buf);
    }
}

static void test_background_fast_failure_returns_sync(void)
{
    char *out = call_bash_background("echo oops; exit 7");
    EXPECT(strstr(out, "oops") != NULL);
    EXPECT(strstr(out, "[exit 7]") != NULL);
    EXPECT(strstr(out, "no task created") != NULL);
    EXPECT(strstr(out, "detached") == NULL);
    free(out);
}

static void test_background_detaches_and_wait_collects(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    setenv("HAX_BASH_TRANSITION_MIN_BYTES", "6", 1); /* "start\n" */
    char *gate = gate_create();
    /* The empty name means unnamed: the detached task still gets an automatic id. */
    char *args = xasprintf("{\"command\":\"echo start; read -r _ <%s; echo done\","
                           "\"background\":true,\"name\":\"\"}",
                           gate);
    char *out = TOOL_BASH.run(args, NULL);
    free(args);
    EXPECT(strstr(out, "[detached as task t") != NULL);
    /* The yield window captured the initial output. */
    EXPECT(strstr(out, "start") != NULL);
    EXPECT(strstr(out, "done") == NULL);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    gate_release(gate);
    out = wait_for_id(id, 30);
    /* Body first, one bracketed status footer after; no command echo, no log path. */
    char *footer = xasprintf("done\n[%s finished (exit 0)", id);
    EXPECT(strstr(out, footer) != NULL);
    free(footer);
    EXPECT(strstr(out, "read -r") == NULL);
    EXPECT(strstr(out, "log:") == NULL);
    free(out);
    free(gate);

    /* Collected tasks are forgotten: the id no longer resolves. */
    out = wait_for_id(id, 1);
    EXPECT(strstr(out, "no such task") != NULL);
    free(out);
    free(id);
    unsetenv("HAX_BASH_TRANSITION_MIN_BYTES");
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_timeout_detaches_instead_of_killing(void)
{
    setenv("HAX_BASH_TIMEOUT", TEST_YIELD, 1);
    setenv("HAX_BASH_TRANSITION_MIN_BYTES", "6", 1); /* "early\n" */
    char *gate = gate_create();
    char *args = xasprintf("{\"command\":\"echo early; read -r _ <%s; echo late\"}", gate);
    char *out = TOOL_BASH.run(args, NULL);
    free(args);
    EXPECT(strstr(out, "[detached as task t") != NULL);
    EXPECT(strstr(out, " timeout]") != NULL);
    EXPECT(strstr(out, "early") != NULL);
    EXPECT(strstr(out, "[timed out") == NULL);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    gate_release(gate);
    out = wait_for_id(id, 30);
    EXPECT(strstr(out, "finished (exit 0)") != NULL);
    EXPECT(strstr(out, "late") != NULL);
    free(out);
    free(id);
    free(gate);
    unsetenv("HAX_BASH_TRANSITION_MIN_BYTES");
    unsetenv("HAX_BASH_TIMEOUT");
}

static void test_kill_stops_process_tree(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char path[] = "/tmp/hax-test-task-pid-XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    close(fd);

    /* The shell's own stderr is discarded so that only the task's output counts towards the footer
     * below: OpenBSD's ksh announces "Terminated" when a foreground job dies of a signal, where
     * dash and FreeBSD's sh stay silent. */
    char *cmd = xasprintf("echo $$ > %s; exec 2>/dev/null; sleep 30", path);
    char *args = xasprintf("{\"command\":\"%s\",\"background\":true}", cmd);
    char *out = TOOL_BASH.run(args, NULL);
    free(args);
    free(cmd);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    /* The kill must not beat the shell to its pid write. */
    int pid = await_pid_file(path);
    unlink(path);
    EXPECT(pid > 0);

    out = kill_id(id);
    /* One status footer; a task that never wrote anything reads "no new output". */
    EXPECT(strstr(out, "killed (signal ") != NULL);
    EXPECT(strstr(out, "; no new output]") != NULL);
    free(out);
    free(id);

    /* ESRCH on Linux or EPERM on Darwin means the process is gone. */
    int alive = pid > 0 && kill(pid, 0) == 0;
    if (alive)
        kill(pid, SIGKILL);
    EXPECT(!alive);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_background_orphans_killed_at_yield(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    setenv("HAX_BASH_TRANSITION_MIN_BYTES", "2", 1); /* the pid line */
    /* The shell exits at once while the orphan inherits the pipe and holds it open, so the
     * yield deadline fires with nothing adoptable. */
    char *out = call_bash_background("sleep 30 & echo $!");
    EXPECT(strstr(out, "detached") == NULL);
    EXPECT(strstr(out, "[timed out") == NULL);
    EXPECT(strstr(out, "[orphaned processes killed after ") != NULL);
    int pid = atoi(out);
    EXPECT(pid > 0);
    EXPECT(process_is_gone(pid));
    free(out);
    unsetenv("HAX_BASH_TRANSITION_MIN_BYTES");
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_background_orphans_killed_at_eof(void)
{
    /* With output redirected away the pipe hits EOF immediately and the shell exits, so the
     * call returns a plain synchronous result — the orphan must still die with it. */
    char *out = call_bash_background("sleep 30 >/dev/null 2>&1 & echo $!");
    EXPECT(strstr(out, "detached") == NULL);
    EXPECT(strstr(out, "orphaned") == NULL);
    int pid = atoi(out);
    EXPECT(pid > 0);
    EXPECT(process_is_gone(pid));
    free(out);
}

static void test_adopted_orphans_killed_at_shell_exit(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *gate = gate_create();
    /* The shell outlives the yield (adopted), then exits leaving the orphan holding the
     * pipe; the registry must kill it at the shell-exit boundary, like the launch path. */
    char *cmd = xasprintf("read -r _ <%s; sleep 30 & echo $!", gate);
    char *out = call_bash_background(cmd);
    free(cmd);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    gate_release(gate);
    free(gate);
    out = wait_for_id(id, 5);
    EXPECT(strstr(out, "finished (exit 0)") != NULL);
    /* The status must not read as a clean finish when descendants were forcibly killed. */
    EXPECT(strstr(out, "; orphaned processes killed") != NULL);
    int pid = atoi(out);
    EXPECT(pid > 0);
    EXPECT(process_is_gone(pid));
    free(out);
    free(id);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_task_fds_not_inherited_by_later_commands(void)
{
    if (access("/proc/self/fd", R_OK) != 0)
        T_SKIP("requires /proc");
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *gate = gate_create();
    char *cmd = xasprintf("read -r _ <%s", gate);
    char *out = call_bash_background(cmd);
    free(cmd);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    /* The live task holds a pipe and a bash-*.log spool; neither may leak into new shells. */
    out = TOOL_BASH.run("{\"command\":\"ls -l /proc/self/fd\"}", NULL);
    EXPECT(strstr(out, "bash-") == NULL);
    free(out);

    gate_release(gate);
    free(gate);
    free(wait_for_id(id, 5));
    free(id);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_exit_note_covers_uncollected_tasks(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *out = call_bash_background("sleep 30");
    char *slow_id = extract_task_id(out);
    EXPECT(slow_id != NULL);
    free(out);

    char *note = task_exit_note();
    EXPECT(note != NULL);
    if (note && slow_id) {
        char *expected = xasprintf("[task %s killed at exit]", slow_id);
        EXPECT_STR_EQ(note, expected);
        free(expected);
    }
    free(note);

    /* A finished task with undelivered output gets its final status, with the output the
     * exit destroys marked as discarded rather than advertised as collectable. */
    char *gate = gate_create();
    char *cmd = xasprintf("read -r _ <%s; echo leftover", gate);
    out = call_bash_background(cmd);
    free(cmd);
    char *quick_id = extract_task_id(out);
    EXPECT(quick_id != NULL);
    free(out);
    gate_release(gate);
    free(gate);

    int quick_done = 0;
    time_t start = time(NULL);
    while (!quick_done && time(NULL) - start < 10) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 20 * 1000000L};
        nanosleep(&ts, NULL);
        struct task_info *rows = NULL;
        size_t n = task_list(&rows);
        for (size_t i = 0; i < n; i++)
            if (quick_id && strcmp(rows[i].id, quick_id) == 0 && !rows[i].running)
                quick_done = 1;
        free(rows);
    }
    EXPECT(quick_done);

    note = task_exit_note();
    EXPECT(note != NULL);
    if (note && slow_id && quick_id) {
        char *finished = xasprintf("[task %s finished (exit 0)", quick_id);
        EXPECT(strstr(note, finished) != NULL);
        free(finished);
        EXPECT(strstr(note, " output discarded]") != NULL);
        char *killed = xasprintf("[task %s killed at exit]", slow_id);
        EXPECT(strstr(note, killed) != NULL);
        free(killed);
    }
    free(note);

    free(wait_for_id(quick_id ? quick_id : "?", 5));
    free(kill_id(slow_id ? slow_id : "?"));
    EXPECT(task_exit_note() == NULL);
    free(slow_id);
    free(quick_id);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_kill_escalates_past_term_exiting_shell(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    setenv("HAX_BASH_TRANSITION_MIN_BYTES", "2", 1); /* the pid line */
    /* SIGTERM ends the shell at once while the child ignores it (SIG_IGN survives the exec);
     * only the SIGKILL escalation after the grace can end the child, and it must fire
     * although the shell is gone. */
    char *out = call_bash_background("sh -c 'trap \\\"\\\" TERM; exec sleep 30' & echo $!; wait");
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    int child_pid = atoi(out);
    EXPECT(child_pid > 0);
    free(out);

    out = kill_id(id ? id : "?");
    EXPECT(strstr(out, "killed (signal ") != NULL);
    free(out);
    EXPECT(process_is_gone(child_pid));
    free(id);
    unsetenv("HAX_BASH_TRANSITION_MIN_BYTES");
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_kill_grace_covers_redirected_cleanup(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    setenv("HAX_BASH_TIMEOUT_GRACE", TEST_GRACE, 1);
    char path[] = "/tmp/hax-test-task-cleanup-XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    close(fd);

    /* The shell dies on SIGTERM at once (pipe EOF included: the child's output is
     * redirected), yet the child's TERM cleanup must still get the grace window. */
    char *ready = xasprintf("%s/ready", t_tempdir());
    char *cmd = xasprintf("sh -c 'trap \\\"sleep " TEST_PAUSE "; echo bye > %s\\\" TERM; "
                          "sh -c \\\"echo $$ > %s; exec sleep 30\\\"' >/dev/null 2>&1 & wait",
                          path, ready);
    char *out = call_bash_background(cmd);
    free(cmd);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    /* The kill must not beat the trapping shell to installing its trap, nor land in its
     * sleep's fork-to-exec window where the inherited trap disposition would consume the
     * TERM; the ready write comes from the exec'd inner shell, past both. */
    EXPECT(await_pid_file(ready) > 0);
    free(ready);

    out = kill_id(id ? id : "?");
    EXPECT(strstr(out, "killed (signal ") != NULL);
    free(out);
    free(id);

    char *content = fs_read_file(path, NULL);
    EXPECT(content != NULL);
    if (content)
        EXPECT_STR_EQ(content, "bye\n");
    free(content);
    unlink(path);
    setenv("HAX_BASH_TIMEOUT_GRACE", TEST_YIELD, 1);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_task_list_snapshots_running_task(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *out = call_bash_background("sleep 30");
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    struct task_info *rows = NULL;
    size_t n = task_list(&rows);
    EXPECT(n >= 1);
    int found = 0;
    for (size_t i = 0; i < n; i++) {
        if (id && strcmp(rows[i].id, id) == 0) {
            found = 1;
            EXPECT(rows[i].running);
            EXPECT(strstr(rows[i].command, "sleep 30") != NULL);
        }
    }
    EXPECT(found);
    free(rows);
    EXPECT(task_running_count() >= 1);

    free(kill_id(id ? id : "?"));
    free(id);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_shutdown_kills_running_tasks(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char path[] = "/tmp/hax-test-task-shutdown-XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    close(fd);

    char *cmd = xasprintf("echo $$ > %s; sleep 30", path);
    char *args = xasprintf("{\"command\":\"%s\",\"background\":true}", cmd);
    char *out = TOOL_BASH.run(args, NULL);
    EXPECT(strstr(out, "detached as task t") != NULL);
    free(args);
    free(cmd);
    free(out);

    /* The shutdown must not beat the shell to its pid write. */
    int pid = await_pid_file(path);
    unlink(path);
    EXPECT(pid > 0);

    task_registry_shutdown();
    EXPECT(task_running_count() == 0);

    int alive = pid > 0 && kill(pid, 0) == 0;
    if (alive)
        kill(pid, SIGKILL);
    EXPECT(!alive);

    /* Numbering restarts with the emptied registry: the next conversation counts from t1. */
    char *gate = gate_create();
    cmd = xasprintf("read -r _ <%s", gate);
    out = call_bash_background(cmd);
    free(cmd);
    EXPECT(strstr(out, "[detached as task t1]") != NULL);
    free(out);
    gate_release(gate);
    free(gate);
    free(wait_for_id("t1", 5));
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_finalize_tasks_resolves_record(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *out = call_bash_background("sleep 30");
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    /* NULL logs are valid; only the in-memory record is asserted here. */
    struct agent_session session = {0};
    agent_finalize_tasks(&session, NULL, NULL);
    EXPECT(session.n_items == 1);
    if (session.n_items == 1 && id) {
        EXPECT(session.items[0].kind == ITEM_USER_MESSAGE);
        EXPECT(session.items[0].origin == ITEM_ORIGIN_TASK_NOTE);
        char *expected = xasprintf("[task %s killed at exit]", id);
        EXPECT_STR_EQ(session.items[0].text, expected);
        free(expected);
    }
    EXPECT(task_running_count() == 0);

    /* Nothing left to resolve: a repeated finalize appends no note. */
    agent_finalize_tasks(&session, NULL, NULL);
    EXPECT(session.n_items == 1);

    for (size_t i = 0; i < session.n_items; i++)
        item_free(&session.items[i]);
    free(session.items);
    free(id);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

/* Fatal-signal handlers cannot run registry code, so the published pgid table alone must be
 * enough to take live task groups down. */
static void test_fatal_hook_kills_task_groups(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    setenv("HAX_BASH_TRANSITION_MIN_BYTES", "2", 1); /* the pid line */
    /* Probe a group member rather than the shell: the killed shell stays an unreaped zombie
     * (still answering kill(pid, 0)) until the registry polls it. */
    char *out = call_bash_background("sleep 30 & echo $!; wait");
    EXPECT(strstr(out, "detached as task t") != NULL);
    int pid = atoi(out);
    EXPECT(pid > 0);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    bash_shell_pgids_kill();
    EXPECT(process_is_gone(pid));
    free(wait_for_id(id, 5));
    free(id);
    unsetenv("HAX_BASH_TRANSITION_MIN_BYTES");
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_running_task_cap_enforced(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    setenv("HAX_TASK_MAX_RUNNING", "1", 1);
    char *gate = gate_create();
    char *cmd = xasprintf("read -r _ <%s", gate);
    char *out = call_bash_background(cmd);
    free(cmd);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    /* A further background request is refused before anything runs. */
    out = call_bash_background("echo never");
    EXPECT(strstr(out, "too many running tasks (max 1)") != NULL);
    EXPECT(strstr(out, "never") == NULL);
    free(out);

    /* At the cap a timed-out command cannot detach; it reverts to kill-at-timeout. */
    setenv("HAX_BASH_TIMEOUT", TEST_YIELD, 1);
    out = TOOL_BASH.run("{\"command\":\"sleep 30\"}", NULL);
    EXPECT(strstr(out, "[timed out after ") != NULL);
    EXPECT(strstr(out, "detached") == NULL);
    free(out);
    unsetenv("HAX_BASH_TIMEOUT");

    /* Collecting the running task frees the slot. */
    gate_release(gate);
    free(gate);
    free(wait_for_id(id, 5));
    free(id);
    setenv("HAX_BASH_TRANSITION_MIN_BYTES", "6", 1); /* "again\n" */
    out = call_bash_background("echo again");
    EXPECT(strstr(out, "again") != NULL);
    EXPECT(strstr(out, "too many running tasks") == NULL);
    free(out);
    unsetenv("HAX_BASH_TRANSITION_MIN_BYTES");
    unsetenv("HAX_TASK_MAX_RUNNING");
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_no_tasks_disables_background_and_tools(void)
{
    setenv("HAX_NO_TASKS", "1", 1);
    char *out = call_bash_background("echo hi");
    EXPECT(strstr(out, "background tasks are disabled") != NULL);
    free(out);

    out = TOOL_TASK_WAIT.run("{}", NULL);
    EXPECT(strstr(out, "background tasks are disabled") != NULL);
    free(out);

    EXPECT(TOOL_BASH.advertise() != &TOOL_BASH.def);
    EXPECT(TOOL_TASK_WAIT.advertise() == NULL);
    unsetenv("HAX_NO_TASKS");

    EXPECT(TOOL_BASH.advertise() == &TOOL_BASH.def);
    EXPECT(TOOL_TASK_WAIT.advertise() == &TOOL_TASK_WAIT.def);
}

static void test_named_task_round_trip(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *gate = gate_create();
    char *args = xasprintf("{\"command\":\"read -r _ <%s; echo named-done\","
                           "\"background\":true,\"name\":\"demo-job\"}",
                           gate);
    char *out = TOOL_BASH.run(args, NULL);
    free(args);
    EXPECT(strstr(out, "[detached as task demo-job]") != NULL);
    free(out);

    /* Live names are reserved. */
    out = TOOL_BASH.run("{\"command\":\"true\",\"background\":true,\"name\":\"demo-job\"}", NULL);
    EXPECT(strstr(out, "'demo-job' is already in use") != NULL);
    free(out);

    gate_release(gate);
    out = wait_for_id("demo-job", 30);
    EXPECT(strstr(out, "named-done") != NULL);
    EXPECT(strstr(out, "[demo-job finished (exit 0)") != NULL);
    free(out);

    /* Collection releases the name for reuse; the fifo is reusable the same way. */
    args = xasprintf("{\"command\":\"read -r _ <%s; echo again\","
                     "\"background\":true,\"name\":\"demo-job\"}",
                     gate);
    out = TOOL_BASH.run(args, NULL);
    free(args);
    EXPECT(strstr(out, "[detached as task demo-job]") != NULL);
    free(out);
    gate_release(gate);
    out = wait_for_id("demo-job", 30);
    EXPECT(strstr(out, "again") != NULL);
    free(out);
    free(gate);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_task_name_validation(void)
{
    static const struct {
        const char *name_json;
        const char *expected_error;
    } cases[] = {
        {"\"has space\"", "letters, digits"},
        {"\"caf\\u00e9\"", "letters, digits"},
        {"\"t7\"", "automatic task id"},
        {"\"this-name-is-way-too-long-to-accept\"", "too long"},
        {"7", "'name' must be a string"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *args = xasprintf("{\"command\":\"echo ran\",\"background\":true,\"name\":%s}",
                               cases[i].name_json);
        char *out = TOOL_BASH.run(args, NULL);
        EXPECT(strstr(out, cases[i].expected_error) != NULL);
        /* Rejected before the command started. */
        EXPECT(strstr(out, "ran") == NULL);
        free(out);
        free(args);
    }

    /* A name at exactly the cap is accepted. */
    char *out = TOOL_BASH.run("{\"command\":\"echo ran\",\"background\":true,"
                              "\"name\":\"abcdefghijklmnopqrstuvwxyz-01234\"}",
                              NULL);
    EXPECT(strstr(out, "ran") != NULL);
    EXPECT(strstr(out, "too long") == NULL);
    free(out);

    /* An empty name means unnamed, not an error: models routinely send every declared field. */
    out = TOOL_BASH.run("{\"command\":\"echo ran\",\"background\":false,\"name\":\"\"}", NULL);
    EXPECT(strstr(out, "ran") != NULL);
    EXPECT(strstr(out, "task") == NULL);
    free(out);
    out = TOOL_BASH.run("{\"command\":\"echo ran\",\"background\":true,\"name\":\"\"}", NULL);
    EXPECT(strstr(out, "ran") != NULL);
    EXPECT(strstr(out, "no task created") != NULL);
    free(out);
}

int main(void)
{
    /* Kill waits sit out the full SIGTERM grace, so the default 2s would dominate the
     * suite; tests needing a real grace window override and restore this. */
    setenv("HAX_BASH_TIMEOUT_GRACE", TEST_YIELD, 1);
    test_background_fast_command_returns_sync();
    test_background_fast_failure_returns_sync();
    test_background_detaches_and_wait_collects();
    test_timeout_detaches_instead_of_killing();
    test_kill_stops_process_tree();
    test_background_orphans_killed_at_yield();
    test_background_orphans_killed_at_eof();
    test_adopted_orphans_killed_at_shell_exit();
    test_task_fds_not_inherited_by_later_commands();
    test_exit_note_covers_uncollected_tasks();
    test_kill_escalates_past_term_exiting_shell();
    test_kill_grace_covers_redirected_cleanup();
    test_task_list_snapshots_running_task();
    test_shutdown_kills_running_tasks();
    test_finalize_tasks_resolves_record();
    test_fatal_hook_kills_task_groups();
    test_running_task_cap_enforced();
    test_no_tasks_disables_background_and_tools();
    test_named_task_round_trip();
    test_task_name_validation();
    task_registry_shutdown();
    T_REPORT();
}
