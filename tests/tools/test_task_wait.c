/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "harness.h"
#include "tool.h"
#include "xalloc.h"
#include "system/fs.h"
#include "tools/task_helpers.h"
#include "tools/task_registry.h"

static void test_wait_streams_output_live(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *gate = gate_create();
    /* The pause spaces the two lines apart so both arrive while the wait streams. */
    char *cmd =
        xasprintf("read -r _ <%s; echo streamed-line; sleep " TEST_PAUSE "; echo final-line", gate);
    char *out = call_bash_background(cmd);
    free(cmd);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    gate_release(gate);
    free(gate);
    struct display_capture capture = {0};
    buf_init(&capture.buf);
    char *args = xasprintf("{\"id\":\"%s\",\"timeout_seconds\":30}", id);
    struct tool_run_ctx ctx = {.display = append_display, .display_data = &capture};
    out = TOOL_TASK_WAIT.run(args, &ctx);
    free(args);

    EXPECT(strstr(out, "streamed-line") != NULL);
    EXPECT(strstr(out, "final-line") != NULL);
    /* The display saw the same body plus the bracketed footer, like a sync bash stream. */
    EXPECT(capture.buf.data != NULL);
    if (capture.buf.data) {
        EXPECT(strstr(capture.buf.data, "streamed-line") != NULL);
        EXPECT(strstr(capture.buf.data, "final-line") != NULL);
        EXPECT(strstr(capture.buf.data, "finished (exit 0)") != NULL);
    }
    free(out);
    free(id);
    buf_free(&capture.buf);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_wait_times_out_on_running_task(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    setenv("HAX_TASK_WAIT_TIMEOUT", "50ms", 1);
    char *out = call_bash_background("sleep 5");
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    out = wait_for_id(id, 0); /* configured 50ms timeout */
    EXPECT(strstr(out, "still running (") != NULL);
    EXPECT(strstr(out, "— wait timed out]") != NULL);
    EXPECT(strstr(out, "; no new output") != NULL);
    free(out);

    /* An explicit 0 is a non-blocking check, not an argument error. */
    char *args = xasprintf("{\"id\":\"%s\",\"timeout_seconds\":0}", id);
    out = TOOL_TASK_WAIT.run(args, NULL);
    free(args);
    EXPECT(strstr(out, "still running (") != NULL);
    EXPECT(strstr(out, "— wait timed out]") != NULL);
    free(out);

    out = kill_id(id);
    EXPECT(strstr(out, "killed (signal ") != NULL);
    free(out);
    free(id);
    unsetenv("HAX_TASK_WAIT_TIMEOUT");
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_wait_returns_early_when_other_task_finishes(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *out = call_bash_background("sleep 30");
    char *slow_id = extract_task_id(out);
    EXPECT(slow_id != NULL);
    free(out);
    char *gate = gate_create();
    char *cmd = xasprintf("read -r _ <%s; echo quick-done", gate);
    out = call_bash_background(cmd);
    free(cmd);
    char *quick_id = extract_task_id(out);
    EXPECT(quick_id != NULL);
    free(out);

    gate_release(gate);
    free(gate);
    out = wait_for_id(slow_id, 30);
    EXPECT(strstr(out, "still running (") != NULL);
    EXPECT(strstr(out, "— another task finished") != NULL);
    free(out);

    /* The finished task is announced by the note and remains collectable. */
    char *note = task_collect_notes();
    EXPECT(note != NULL);
    if (note) {
        char *header = xasprintf("[task %s finished (exit 0)", quick_id);
        EXPECT(strstr(note, header) != NULL);
        free(header);
        /* Announce-only: status and pending size, never the output body or command. */
        EXPECT(strstr(note, "quick-done") == NULL);
        EXPECT(strstr(note, "sleep") == NULL);
        EXPECT(strstr(note, " output]") != NULL);
    }
    free(note);

    out = wait_for_id(quick_id, 1);
    EXPECT(strstr(out, "quick-done") != NULL);
    EXPECT(strstr(out, "finished (exit 0)") != NULL);
    free(out);

    free(kill_id(slow_id));
    free(slow_id);
    free(quick_id);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_note_with_no_output_is_collected_outright(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *gate = gate_create();
    char *cmd = xasprintf("read -r _ <%s", gate);
    char *out = call_bash_background(cmd);
    free(cmd);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    gate_release(gate);
    free(gate);
    char *note = NULL;
    time_t start = time(NULL);
    while (!note && time(NULL) - start < 10) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 20 * 1000000L};
        nanosleep(&ts, NULL);
        note = task_collect_notes();
    }
    EXPECT(note != NULL);
    if (note)
        EXPECT(strstr(note, "; no output]") != NULL);
    free(note);

    /* Nothing left to collect, so the task was swept with the note. */
    out = wait_for_id(id, 1);
    EXPECT(strstr(out, "no such task") != NULL);
    free(out);
    free(id);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_kill_delivers_pending_output(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *first_gate = gate_create();
    char *last_gate = gate_create();
    char *cmd =
        xasprintf("read -r _ <%s; echo pending-output; read -r _ <%s", first_gate, last_gate);
    char *out = call_bash_background(cmd);
    free(cmd);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    gate_release(first_gate);
    free(first_gate);
    /* Let post-detach output land in the spool before killing; the task stays blocked on the
     * second gate, which is never released. */
    int has_output = 0;
    time_t start = time(NULL);
    while (!has_output && time(NULL) - start < 10) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 20 * 1000000L};
        nanosleep(&ts, NULL);
        struct task_info *rows = NULL;
        size_t n = task_list(&rows);
        for (size_t i = 0; i < n; i++)
            if (id && strcmp(rows[i].id, id) == 0 && rows[i].total_bytes > 0)
                has_output = 1;
        free(rows);
    }
    EXPECT(has_output);

    /* One call: the kill, the pending output, and the final status. */
    out = kill_id(id);
    EXPECT(strstr(out, "pending-output") != NULL);
    EXPECT(strstr(out, "killed (signal ") != NULL);
    free(out);

    /* Kill-and-collect forgets the task. */
    out = wait_for_id(id, 1);
    EXPECT(strstr(out, "no such task") != NULL);
    free(out);
    free(id);
    free(last_gate);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_kill_fires_at_wait_deadline(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *out = call_bash_background("sleep 30");
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    char *args = xasprintf("{\"id\":\"%s\",\"timeout_seconds\":1,\"kill\":true}", id);
    out = TOOL_TASK_WAIT.run(args, NULL);
    free(args);
    EXPECT(strstr(out, "killed (signal ") != NULL);
    EXPECT(strstr(out, "wait timed out") == NULL);
    free(out);
    free(id);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_kill_spares_task_finishing_within_timeout(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *gate = gate_create();
    char *cmd = xasprintf("read -r _ <%s; echo done-first", gate);
    char *out = call_bash_background(cmd);
    free(cmd);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    gate_release(gate);
    free(gate);
    char *args = xasprintf("{\"id\":\"%s\",\"timeout_seconds\":30,\"kill\":true}", id);
    out = TOOL_TASK_WAIT.run(args, NULL);
    free(args);
    EXPECT(strstr(out, "done-first") != NULL);
    EXPECT(strstr(out, "finished (exit 0)") != NULL);
    /* Match the status phrase, not a bare "killed": the footer can also carry an orphan-sweep
     * note containing the word. This command runs only builtins and so orphans nothing — that
     * note appears when the drainer has yet to observe EOF as the shell's exit is seen. */
    EXPECT(strstr(out, "killed (signal ") == NULL);
    free(out);
    free(id);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_kill_now_beats_pending_foreign_completion(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *out = call_bash_background("sleep 30");
    char *slow_id = extract_task_id(out);
    EXPECT(slow_id != NULL);
    free(out);
    char *gate = gate_create();
    char *cmd = xasprintf("read -r _ <%s; echo quick-done", gate);
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

    /* An immediate kill must fire, not yield to the pending foreign completion. */
    out = kill_id(slow_id);
    EXPECT(strstr(out, "killed (signal ") != NULL);
    EXPECT(strstr(out, "another task finished") == NULL);
    free(out);

    free(wait_for_id(quick_id, 1));
    free(slow_id);
    free(quick_id);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_foreign_completion_ends_kill_wait_without_killing(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *out = call_bash_background("sleep 30");
    char *slow_id = extract_task_id(out);
    EXPECT(slow_id != NULL);
    free(out);
    char *gate = gate_create();
    char *cmd = xasprintf("read -r _ <%s; echo quick-done", gate);
    out = call_bash_background(cmd);
    free(cmd);
    char *quick_id = extract_task_id(out);
    EXPECT(quick_id != NULL);
    free(out);

    gate_release(gate);
    free(gate);
    /* The kill was armed for the 30s deadline, so the early return must not kill. */
    char *args = xasprintf("{\"id\":\"%s\",\"timeout_seconds\":30,\"kill\":true}", slow_id);
    out = TOOL_TASK_WAIT.run(args, NULL);
    free(args);
    EXPECT(strstr(out, "still running (") != NULL);
    EXPECT(strstr(out, "; not killed") != NULL);
    EXPECT(strstr(out, "— another task finished") != NULL);
    free(out);

    out = kill_id(slow_id);
    EXPECT(strstr(out, "killed (signal ") != NULL);
    free(out);
    free(wait_for_id(quick_id, 1));
    free(slow_id);
    free(quick_id);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_binary_markers_reach_display(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    setenv("HAX_BASH_TRANSITION_MIN_BYTES", "3", 1); /* 'A\0B' */
    char *gate = gate_create();
    /* Binary output is never streamed, so the suppression marker itself must be shown. */
    char *args = xasprintf("{\"command\":\"printf 'A\\\\000B'; read -r _ <%s; printf 'C\\\\000D'\","
                           "\"background\":true}",
                           gate);
    struct display_capture launch_capture = {0};
    buf_init(&launch_capture.buf);
    struct tool_run_ctx launch_ctx = {.display = append_display, .display_data = &launch_capture};
    char *out = TOOL_BASH.run(args, &launch_ctx);
    free(args);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    EXPECT(strstr(out, "[binary output suppressed") != NULL);
    free(out);
    EXPECT(launch_capture.buf.data != NULL &&
           strstr(launch_capture.buf.data, "[binary output suppressed") != NULL);
    buf_free(&launch_capture.buf);

    /* Nothing new arrived, so the marker is not re-reported as fresh output. */
    setenv("HAX_TASK_WAIT_TIMEOUT", "50ms", 1);
    out = wait_for_id(id ? id : "?", 0);
    EXPECT(strstr(out, "[binary output suppressed") == NULL);
    EXPECT(strstr(out, "no new output") != NULL);
    free(out);
    unsetenv("HAX_TASK_WAIT_TIMEOUT");

    gate_release(gate);
    free(gate);
    struct display_capture wait_capture = {0};
    buf_init(&wait_capture.buf);
    args = xasprintf("{\"id\":\"%s\",\"timeout_seconds\":5}", id ? id : "?");
    struct tool_run_ctx wait_ctx = {.display = append_display, .display_data = &wait_capture};
    out = TOOL_TASK_WAIT.run(args, &wait_ctx);
    free(args);
    /* The trailing bytes after the gate are new, so the marker repeats with the new total. */
    EXPECT(strstr(out, "[binary output suppressed: 6 bytes total") != NULL);
    free(out);
    EXPECT(wait_capture.buf.data != NULL &&
           strstr(wait_capture.buf.data, "[binary output suppressed") != NULL);
    EXPECT(wait_capture.buf.data != NULL &&
           strstr(wait_capture.buf.data, "finished (exit 0)") != NULL);
    buf_free(&wait_capture.buf);
    free(id);
    unsetenv("HAX_BASH_TRANSITION_MIN_BYTES");
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_binary_marker_shown_after_streamed_text_at_launch(void)
{
    /* The pause keeps the text and the NUL in separate chunks, so the text streams (and would
     * have swallowed the marker) before binary hits; the held transition keeps both inside
     * the launch window. */
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    setenv("HAX_BASH_TRANSITION_MIN_BYTES", "11", 1); /* "visible\n" + 'A\0B' */
    char *gate = gate_create();
    char *cmd = xasprintf("{\"command\":\"echo visible; sleep " TEST_PAUSE
                          "; printf 'A\\\\000B'; read -r _ <%s\",\"background\":true}",
                          gate);
    struct display_capture capture = {0};
    buf_init(&capture.buf);
    struct tool_run_ctx ctx = {.display = append_display, .display_data = &capture};
    char *out = TOOL_BASH.run(cmd, &ctx);
    free(cmd);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    EXPECT(strstr(out, "[binary output suppressed") != NULL);
    free(out);
    EXPECT(capture.buf.data != NULL && strstr(capture.buf.data, "visible") != NULL);
    EXPECT(capture.buf.data != NULL &&
           strstr(capture.buf.data, "[binary output suppressed") != NULL);
    buf_free(&capture.buf);

    gate_release(gate);
    free(gate);
    free(wait_for_id(id, 5));
    free(id);
    unsetenv("HAX_BASH_TRANSITION_MIN_BYTES");
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_binary_marker_shown_after_streamed_text_in_wait(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *gate = gate_create();
    /* Text streams during the wait first, then the NUL turns the task binary before it ends;
     * the pause keeps the two in separate chunks. */
    char *cmd =
        xasprintf("read -r _ <%s; echo streamed; sleep " TEST_PAUSE "; printf '\\\\000'", gate);
    char *out = call_bash_background(cmd);
    free(cmd);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    gate_release(gate);
    free(gate);
    struct display_capture capture = {0};
    buf_init(&capture.buf);
    char *args = xasprintf("{\"id\":\"%s\",\"timeout_seconds\":30}", id);
    struct tool_run_ctx ctx = {.display = append_display, .display_data = &capture};
    out = TOOL_TASK_WAIT.run(args, &ctx);
    free(args);
    EXPECT(strstr(out, "[binary output suppressed") != NULL);
    free(out);
    EXPECT(capture.buf.data != NULL && strstr(capture.buf.data, "streamed") != NULL);
    EXPECT(capture.buf.data != NULL &&
           strstr(capture.buf.data, "[binary output suppressed") != NULL);
    buf_free(&capture.buf);
    free(id);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_runaway_output_killed_without_polling(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char path[] = "/tmp/hax-test-task-runaway-XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    close(fd);

    /* The producer is the shell's child, not the shell: once killed it is reaped by init
     * (the shell itself would linger as a zombie until a registry poll reaps it). The gate
     * holds the flood until after detach: an ungated producer races the yield window
     * against the drainer reaching the output limit, and on a fast machine the limit can
     * win, completing the call synchronously with no task to wait on. */
    char *gate = gate_create();
    char *cmd = xasprintf("{ read -r _ <%s; exec yes; } & echo $! > %s; wait", gate, path);
    char *args = xasprintf("{\"command\":\"%s\",\"background\":true}", cmd);
    char *out = TOOL_BASH.run(args, NULL);
    free(args);
    free(cmd);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    int pid = await_pid_file(path);
    unlink(path);
    EXPECT(pid > 0);

    gate_release(gate);
    free(gate);
    /* The drainer must stop the producer at the output limit on its own; nothing here calls
     * into the registry until the process is already gone. */
    EXPECT(process_is_gone(pid));

    out = wait_for_id(id, 30);
    EXPECT(strstr(out, "killed (signal ") != NULL);
    EXPECT(strstr(out, "[output limit reached: ") != NULL);
    free(out);
    free(id);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_wait_missing_or_unknown_id(void)
{
    char *out = TOOL_TASK_WAIT.run("{}", NULL);
    EXPECT(strstr(out, "missing 'id'") != NULL);
    free(out);

    out = TOOL_TASK_WAIT.run("{\"id\":\"t999\",\"timeout_seconds\":1}", NULL);
    EXPECT(strstr(out, "no such task: t999") != NULL);
    free(out);

    out = TOOL_TASK_WAIT.run("{\"id\":\"t1\",\"timeout_seconds\":-1}", NULL);
    EXPECT(strstr(out, "'timeout_seconds' must be an integer >= 0") != NULL);
    free(out);

    out = TOOL_TASK_WAIT.run("{\"id\":\"t1\",\"kill\":\"yes\"}", NULL);
    EXPECT(strstr(out, "'kill' must be a boolean") != NULL);
    free(out);
}

static void test_detached_log_holds_full_output(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    char *gate = gate_create();
    char *cmd = xasprintf("echo first; read -r _ <%s; echo second", gate);
    char *out = call_bash_background(cmd);
    free(cmd);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    /* The compact launch footer no longer carries the path; /tasks (task_list) does. */
    EXPECT(strstr(out, "log:") == NULL);
    free(out);

    char log_path[256] = {0};
    struct task_info *rows = NULL;
    size_t n = task_list(&rows);
    for (size_t i = 0; i < n; i++) {
        if (id && strcmp(rows[i].id, id) == 0 && rows[i].spool_path &&
            strlen(rows[i].spool_path) < sizeof(log_path))
            snprintf(log_path, sizeof(log_path), "%s", rows[i].spool_path);
    }
    free(rows);
    EXPECT(*log_path != '\0');

    gate_release(gate);
    free(gate);
    out = wait_for_id(id, 30);
    EXPECT(strstr(out, "second") != NULL);
    free(out);
    if (*log_path) {
        char *content = fs_read_file(log_path, NULL);
        EXPECT(content != NULL);
        if (content) {
            EXPECT_STR_EQ(content, "first\nsecond\n");
            free(content);
        }
    }
    free(id);
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

static void test_large_collection_keeps_head_and_tail(void)
{
    setenv("HAX_BASH_BACKGROUND_YIELD", TEST_YIELD, 1);
    setenv("HAX_TOOL_OUTPUT_CAP", "50k", 1);
    char *gate = gate_create();
    char *cmd = xasprintf("read -r _ <%s; echo FIRST-ERROR; seq 1 20000; echo LAST-LINE", gate);
    char *out = call_bash_background(cmd);
    free(cmd);
    char *id = extract_task_id(out);
    EXPECT(id != NULL);
    free(out);

    gate_release(gate);
    free(gate);
    out = wait_for_id(id, 30);
    const char *head = strstr(out, "FIRST-ERROR");
    const char *marker = strstr(out, "[output truncated: omitted ");
    const char *tail = strstr(out, "LAST-LINE");
    EXPECT(head != NULL);
    EXPECT(marker != NULL);
    EXPECT(tail != NULL);
    EXPECT(head && marker && tail && head < marker && marker < tail);
    /* The gap marker names the log — the only place the path appears. */
    EXPECT(strstr(out, "full output: ") != NULL);
    free(out);
    free(id);
    unsetenv("HAX_TOOL_OUTPUT_CAP");
    unsetenv("HAX_BASH_BACKGROUND_YIELD");
}

int main(void)
{
    /* Kill waits sit out the full SIGTERM grace, so the default 2s would dominate the
     * suite; tests needing a real grace window override and restore this. */
    setenv("HAX_BASH_TIMEOUT_GRACE", TEST_YIELD, 1);
    test_wait_streams_output_live();
    test_wait_times_out_on_running_task();
    test_wait_returns_early_when_other_task_finishes();
    test_note_with_no_output_is_collected_outright();
    test_kill_delivers_pending_output();
    test_kill_fires_at_wait_deadline();
    test_kill_spares_task_finishing_within_timeout();
    test_kill_now_beats_pending_foreign_completion();
    test_foreign_completion_ends_kill_wait_without_killing();
    test_binary_markers_reach_display();
    test_binary_marker_shown_after_streamed_text_at_launch();
    test_binary_marker_shown_after_streamed_text_in_wait();
    test_runaway_output_killed_without_polling();
    test_wait_missing_or_unknown_id();
    test_detached_log_holds_full_output();
    test_large_collection_keeps_head_and_tail();
    task_registry_shutdown();
    T_REPORT();
}
