/* SPDX-License-Identifier: MIT */
#ifndef HAX_SLASH_H
#define HAX_SLASH_H

struct agent_state;

/*
 * Slash-command dispatch.
 *
 * The interactive REPL hands every freshly-read prompt line to
 * slash_dispatch before sending it to the model. If the line begins
 * with `/`, the dispatcher routes it to a registered handler that
 * operates on session state directly — clearing the conversation,
 * printing help, etc. — instead of producing a model turn. Anything
 * else returns SLASH_NOT_A_COMMAND and the caller proceeds as normal.
 *
 * Commands live in a static registry inside slash.c. There is no
 * runtime registration API: to add a command, add an entry there.
 * Aliases (e.g. /clear → /new) are listed alongside the canonical
 * name on the same registry entry.
 *
 * Distinct from src/tools/bash_classify.c, which classifies shell command
 * lines for the bash tool — different domain, similar word.
 */

enum slash_result {
    SLASH_NOT_A_COMMAND, /* line didn't start with '/' — caller falls through to model */
    SLASH_HANDLED,       /* command ran; agent loop should re-prompt */
    SLASH_UNKNOWN,       /* '/<x>' where <x> isn't registered */
    SLASH_BAD_USAGE,     /* known command but with extra args it doesn't accept */
};

struct slash_ctx {
    /* Live REPL state. Defined in agent.h. Single field so handlers
     * touch a coherent snapshot of session+provider+transcript without
     * slash.h needing to grow each time the agent gains a new piece
     * of mutable state. */
    struct agent_state *state;
};

/* If `line` starts with '/', look up a registered command and run it.
 * Prints its own diagnostic for SLASH_UNKNOWN / SLASH_BAD_USAGE; the
 * caller doesn't need to. SLASH_NOT_A_COMMAND is silent. */
enum slash_result slash_dispatch(const char *line, struct slash_ctx *ctx);

#endif /* HAX_SLASH_H */
