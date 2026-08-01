/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_BASH_PROCESS_H
#define HAX_TOOLS_BASH_PROCESS_H

#include <sys/types.h>

#include "tool.h"

/* Run the command and return owned model-facing output, optionally streaming display bytes. When
 * background is set, the command detaches into a task after the configured yield window instead
 * of the timeout. `name` (pre-validated by task_name_error, or NULL) becomes the task id if the
 * command detaches. */
char *bash_run_command(const char *command, long timeout_ms, int background, const char *name,
                       tool_display_fn display, void *display_data);

/* Signal the command's process group, falling back to the pid when the group is not yet (or no
 * longer) alive. The pid must be unreaped. */
void bash_signal_process_tree(pid_t pid, int signal_number);

/* Observe the shell's exit without reaping it, so the zombie keeps the pid and process group
 * reserved for signaling. Sets *exit_seen on exit; returns waitid's result. */
int bash_process_exit_seen(pid_t pid, int *exit_seen);

/* Fatal-signal cleanup registry of live shell process groups. Shells are published at spawn and
 * retracted before the reap that frees the pid for reuse, so a handler firing in between can
 * only ever signal a group hax still owns. Publish/retract run on the tool-dispatch thread;
 * kill is async-signal-safe and may run from a signal handler. */
void bash_shell_pgid_publish(pid_t pid);
void bash_shell_pgid_retract(pid_t pid);
void bash_shell_pgids_kill(void);

#endif /* HAX_TOOLS_BASH_PROCESS_H */
