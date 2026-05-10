/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_CORE_H
#define HAX_AGENT_CORE_H

#include <stddef.h>

#include "provider.h"
#include "tool.h"
#include "turn.h"

/* Run-wide options parsed from CLI flags. Lives here (not in agent.h)
 * so the one-shot path can consume it without dragging in the
 * interactive REPL header. */
struct hax_opts {
    /* Bypass system prompt, env block, AGENTS.md, skills, and tools.
     * Sends only the user's message — useful for testing models on
     * raw tasks or as a barebones chat interface. */
    int raw;
};

const struct tool *find_tool(const char *name);

/* Append `it` into a malloc'd vector, doubling capacity on overflow. The
 * three pointers form the canonical "items" vector used in agent.c and
 * oneshot.c — extracted here so both paths grow it the same way. */
void items_append(struct item **items, size_t *n, size_t *cap, struct item it);

/* Resolve the `reasoning_effort` value to send for this session.
 * Empty HAX_REASONING_EFFORT means "force omit"; unset falls back to
 * the provider default; any non-empty value passes through verbatim. */
const char *resolve_reasoning_effort(const struct provider *p);

/* Build the assembled system prompt (base text + env block + AGENTS.md +
 * skills) for `model`. Returns malloc'd, or NULL when the system message
 * should be omitted entirely.
 *
 *   raw=1               → NULL (--raw means "just the prompt, nothing else")
 *   HAX_SYSTEM_PROMPT=""→ NULL (explicit opt-out, same as before)
 *   HAX_SYSTEM_PROMPT   → that value + agent_env_build_suffix(model)
 *   unset               → built-in default + agent_env_build_suffix(model)
 */
char *build_system_prompt(const char *model, int raw);

/* Live per-run state shared by the interactive and one-shot paths.
 * Owns the items vector, the assembled system prompt, and the tools
 * table; borrows model and reasoning_effort. */
struct agent_session {
    const char *model;            /* borrowed */
    const char *reasoning_effort; /* borrowed; NULL = "omit" */
    char *sys;                    /* owned; NULL = no system message */
    struct tool_def *tools;       /* owned; NULL when n_tools == 0 */
    size_t n_tools;

    struct item *items;
    size_t n_items;
    size_t cap_items;
};

/* Resolve model + build sys + tools table. Returns 0 on success, -1
 * when no model is available (an error is logged to stderr in that
 * case, so the caller can just propagate the failure). */
int agent_session_init(struct agent_session *s, struct provider *p, const struct hax_opts *opts);

void agent_session_free(struct agent_session *s);

/* Drop every conversation item, leaving the session ready to start a
 * fresh turn. Preserves model, system prompt, tools, and reasoning_effort
 * — those are session-level config, not per-conversation state. The
 * items vector's capacity is kept allocated so subsequent appends don't
 * have to grow it from zero. Used by `/new` (and `/clear`). */
void agent_session_reset(struct agent_session *s);

/* Snapshot the session's current state into a `struct context` ready
 * to hand to a provider's stream(). The returned struct borrows the
 * session's pointers — its lifetime ends with the next mutating call
 * on the session. */
struct context agent_session_context(const struct agent_session *s);

/* Append ITEM_USER_MESSAGE preceded by ITEM_TURN_BOUNDARY. Used at the
 * start of each user input. */
void agent_session_add_user(struct agent_session *s, const char *text);

/* Append ITEM_TURN_BOUNDARY only — used between consecutive model
 * round-trips inside one user turn (after the first one) so the
 * transcript renderer can mark each round as its own boundary. */
void agent_session_add_boundary(struct agent_session *s);

/* Drain finished items from `t` into the session. Reports:
 *   *out_before        = session's n_items just before the drain
 *   *out_had_tool_call = 1 iff any newly-drained item is a tool call
 * Does NOT call turn_reset — the caller still owns the turn and is
 * expected to reset it after this returns. Both existing callers check
 * t->error before absorbing and then unconditionally turn_reset, so
 * absorb only handles the success case. */
void agent_session_absorb(struct agent_session *s, struct turn *t, size_t *out_before,
                          int *out_had_tool_call);

#endif /* HAX_AGENT_CORE_H */
