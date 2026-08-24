/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_BASH_PROCESS_H
#define HAX_TOOLS_BASH_PROCESS_H

#include "tool.h"

struct spawn_process;

/* Run the command and return owned model-facing output, streaming display bytes through `ctx`
 * (NULL disables display). When background is set, the command detaches into a task after the
 * configured yield window instead of the timeout. `name` (pre-validated by task_name_error, or
 * NULL) becomes the task id if the command detaches. A model-only annotation ending the output
 * is reported via ctx->output_hidden_tail. */
char *bash_run_command(const char *command, long timeout_ms, int background, const char *name,
                       struct tool_run_ctx *ctx);

/* Fatal-signal cleanup registry. Processes remain published until the wait consumes them. */
void bash_live_process_publish(struct spawn_process *process);
void bash_live_process_retract(struct spawn_process *process);
void bash_shell_pgids_kill(void);

#endif /* HAX_TOOLS_BASH_PROCESS_H */
