/* SPDX-License-Identifier: MIT */
#ifndef HAX_SLASH_H
#define HAX_SLASH_H

struct agent_state;

enum slash_result {
    SLASH_NOT_A_COMMAND,
    SLASH_HANDLED,
    SLASH_UNKNOWN,
    SLASH_BAD_USAGE,
};

/* Dispatch `line` when it starts with a slash and a bareword command name. Non-command input is
 * silent; every other result consumes the line and prints any required diagnostic. `state` and its
 * renderer must be live for consumed input. */
enum slash_result slash_dispatch(const char *line, struct agent_state *state);

#endif /* HAX_SLASH_H */
