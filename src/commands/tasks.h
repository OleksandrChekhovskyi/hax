/* SPDX-License-Identifier: MIT */
#ifndef HAX_COMMANDS_TASKS_H
#define HAX_COMMANDS_TASKS_H

struct completion;

/* /tasks: list the background tasks, or stop them for "kill <id>..." or "kill all". */
void tasks_command(const char *argument);

/* Add the values of the /tasks argument word after `preceding`, the trimmed earlier words, for
 * Tab completion: "kill" first, then "all" while no task is named, and the running tasks not
 * named yet. */
void tasks_choices(const char *preceding, struct completion *choices);

#endif /* HAX_COMMANDS_TASKS_H */
