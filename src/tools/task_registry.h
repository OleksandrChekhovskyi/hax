/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_TASK_REGISTRY_H
#define HAX_TOOLS_TASK_REGISTRY_H

#include <stddef.h>
#include <sys/types.h>

#include "tool.h"

/* Process-local registry of background tasks: shell commands that outlived their foreground
 * window and keep running detached from the tool call that started them. Each task owns the
 * child's process tree, the read end of its output pipe, and a spool file that accumulates the
 * complete output; a per-task drainer thread pumps pipe to spool.
 *
 * All entry points below run on the agent loop thread. Tasks never outlive the process:
 * task_registry_shutdown kills every remaining process tree.
 *
 * Output is delivered per task, only through task_wait_stream (and the launch report), capped
 * like synchronous tool output with a per-task cursor tracking what the model has seen.
 * Completion is announced separately by one-line status notes; an announced task stays
 * collectable until a wait delivers its remaining output, and only then is it forgotten. The
 * spool file stays on disk as the overflow escape hatch. */

/* Reject a proposed task name with an allocated recoverable error, or return NULL when it is
 * usable: short identifier characters only, not the reserved t<digits> shape, and not held by a
 * live task. Meant to run before the command starts, so a bad name has no side effects. */
char *task_name_error(const char *name);

/* Adopt a still-running command that was started at started_ms. `name` (validated by
 * task_name_error, or NULL) becomes the task id; unnamed tasks get "tN". spooled_bytes is what
 * the foreground window already wrote to the spool; binary marks NUL bytes seen so far;
 * pipe_eof records that the pipe already drained (the child closed its output but has not
 * exited). Returns the registry-owned id after taking ownership of the process, pipe_fd,
 * spool_fd, and spool_path, or NULL when the drainer thread cannot start — the caller then
 * retains ownership of all of them and should fall back to killing the command. */
const char *task_adopt(pid_t pid, int pipe_fd, const char *command, const char *name,
                       long started_ms, int spool_fd, char *spool_path, size_t spooled_bytes,
                       int binary, int pipe_eof);

/* Return the bare output body produced since the last report (empty when there is none),
 * advancing the delivery cursor. *marker_out receives an owned withheld-content note (binary,
 * spool failure, overflow) or NULL; unlike the body it is never carried by a live stream, so
 * callers must send it to their display themselves. Returns NULL for an unknown id. */
char *task_report_output(const char *id, char **marker_out);

/* Wait on one task, forwarding its output live through `display` while blocking. Returns the
 * capped undelivered output plus a bracketed status footer. Ends when the task finishes, when
 * any unannounced task finishes (so its note rides this round trip), on timeout, or on Esc; a
 * wait on an already-finished task returns its remaining output immediately and forgets it.
 * Never returns NULL. */
char *task_wait_stream(const char *id, long timeout_ms, tool_display_fn display,
                       void *display_data);

/* Stop the named tasks: SIGTERM, the bash grace window, then SIGKILL. Reports one status line
 * per task; undelivered output stays collectable via task_wait_stream. Never returns NULL. */
char *task_kill_report(const char *const *ids, size_t n_ids);

/* User-driven stop that does not consume the model-facing report: the next collected note still
 * delivers each task's final state. NULL/empty ids stops every live task. Returns the number of
 * tasks signalled (unknown or already-finished ids are skipped). */
size_t task_stop(const char *const *ids, size_t n_ids);

/* One announce-only note (a one-line status per newly finished task), or NULL when there are
 * none. Announced tasks stay collectable until a wait delivers their output. */
char *task_collect_notes(void);

/* One allocated line per uncollected task: the final status (noting undelivered output as
 * discarded) for finished tasks, and a killed-at-exit line for those task_registry_shutdown is
 * about to kill. NULL when everything was collected. Appended to the conversation before
 * shutdown so a resumed session sees every detached command resolved and nothing advertised as
 * still collectable. */
char *task_exit_note(void);

/* Borrowed snapshot row for /tasks. */
struct task_info {
    const char *id;
    const char *command;
    const char *spool_path; /* NULL when the spool could not be written */
    int running;
    int exit_code;   /* valid when !running and the command exited */
    int term_signal; /* signal that ended it; 0 otherwise */
    long elapsed_ms;
    size_t total_bytes;
};

/* Fill an allocated array of live views (invalidated by any other registry call). The caller
 * frees only the array. Returns the row count. */
size_t task_list(struct task_info **rows_out);

size_t task_running_count(void);

/* Kill every remaining process tree, join drainers, and free all state. Idempotent. */
void task_registry_shutdown(void);

#endif /* HAX_TOOLS_TASK_REGISTRY_H */
