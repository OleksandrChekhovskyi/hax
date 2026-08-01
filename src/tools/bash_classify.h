/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_BASH_CLASSIFY_H
#define HAX_TOOLS_BASH_CLASSIFY_H

/* Return nonzero when `command` appears to be read-only exploration. This heuristic selects only
 * the display preview mode; it never changes execution or model-facing output. */
int bash_command_is_exploration(const char *command);

#endif /* HAX_TOOLS_BASH_CLASSIFY_H */
