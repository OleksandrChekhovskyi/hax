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

/* Complete the partial command name `prefix` (without the slash) like a shell: return the unique
 * matching name or alias followed by a space, or the longest prefix shared by every match. Return
 * NULL when nothing longer than `prefix` exists. The result is malloc'd. */
char *slash_complete_name(const char *prefix);

/* Return the malloc'd space-separated commands whose name or alias starts with `prefix`, each with
 * its slash and in /help order, or NULL when fewer than two match. */
char *slash_name_candidates(const char *prefix);

/* Return the malloc'd argument placeholder for the prompt `line` while its cursor sits at the end
 * of a complete command name with no argument yet, or NULL. */
char *slash_hint(const char *line);

/* Tab completion of the command name at the start of the prompt. */
struct input_completer;
extern const struct input_completer slash_completer;

#endif /* HAX_SLASH_H */
