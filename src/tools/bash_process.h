/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_BASH_PROCESS_H
#define HAX_TOOLS_BASH_PROCESS_H

#include "tool.h"

/* Run the command and return owned model-facing output, optionally streaming display bytes. */
char *bash_run_command(const char *command, long timeout_ms, tool_display_fn display,
                       void *display_data);

#endif /* HAX_TOOLS_BASH_PROCESS_H */
