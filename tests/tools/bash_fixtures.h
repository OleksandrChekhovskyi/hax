/* SPDX-License-Identifier: MIT */
#ifndef HAX_TESTS_TOOLS_BASH_FIXTURES_H
#define HAX_TESTS_TOOLS_BASH_FIXTURES_H

#include <stddef.h>

#include "buf.h"

/* Fixtures shared by the bash and task tool tests. Returned strings are allocated and owned by the
 * caller. */

/* Producers must outlive the yield window to detach. Tests that also need initial output captured
 * before the transition hold it open with HAX_BASH_TRANSITION_MIN_BYTES instead of betting a
 * widened window against spawn latency. */
#define TEST_YIELD "10ms"
/* Kills sit out the whole SIGTERM grace; above zero so the SIGTERM path still runs. Tests that need
 * cleanup to finish inside the grace set their own. */
#define TEST_KILL_GRACE "10ms"

/* Run the bash tool with `background` set; `escaped_command` is already JSON-escaped. */
char *call_bash_background(const char *escaped_command);

/* Return the "tN" id from a detachment report, or NULL. */
char *extract_task_id(const char *result);

/* Wait on task `id` through the task_wait tool; a timeout of 0 takes the configured default. */
char *wait_for_id(const char *id, int timeout_seconds);

/* Immediate kill-and-collect: task_wait with `kill` and no timeout. */
char *kill_id(const char *id);

/* Return 1 once `pid` is gone, allowing ten seconds for a kill to land and the orphan to be reaped;
 * otherwise SIGKILL it and return 0. A nonpositive pid returns 0. */
int process_is_gone(int pid);

/* Wait up to ten seconds for a command to write its pid into `path`, so the test cannot act on the
 * process tree before it got that far. Returns the pid, or -1. */
int await_pid_file(const char *path);

/* A FIFO a task blocks on with `read -r _ <gate`: it holds the task alive across the yield window
 * without timers, and releasing it lets the task finish at once. */
char *gate_create(void);

/* Let one reader past the gate. Waits up to ten seconds for the reader to open it, so a task that
 * failed to start fails the test instead of hanging it. */
void gate_release(const char *path);

/* Display output collected by append_display, a tool_display_fn taking the capture as its data. */
struct display_capture {
    struct buf buf;
    /* Optional: release `release_gate` once the display has shown `release_on`, so a task can hold
     * later output until earlier output provably streamed. */
    const char *release_on;
    const char *release_gate;
};

void append_display(const char *bytes, size_t len, void *data);

#endif /* HAX_TESTS_TOOLS_BASH_FIXTURES_H */
