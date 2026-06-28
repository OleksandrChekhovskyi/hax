/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_CORE_H
#define HAX_AGENT_CORE_H

#include <stddef.h>

#include "provider.h"
#include "tool.h"
#include "turn.h"

/* Marker stored in history when a turn was cut short. Shared by history shaping
 * and tool-dispatch rendering so both emit the same recoverable signal. */
#define INTERRUPT_MARKER "[interrupted]"

/* CLI options shared by interactive and one-shot runs. */
struct hax_opts {
    /* Bypass system prompt, env block, AGENTS.md, skills, and tools. */
    int raw;
    /* Session .jsonl to load, then append to; NULL starts a fresh session. */
    const char *resume_path;
};

const struct tool *find_tool(const char *name);

/* Append `it` into the shared malloc'd items vector, growing as needed. */
void items_append(struct item **items, size_t *n, size_t *cap, struct item it);

/* Resolve the `reasoning_effort` value to send for this provider. */
const char *resolve_reasoning_effort(const struct provider *p);

/* Build the malloc'd system prompt, or NULL when system text is disabled.
 * raw=1 and HAX_SYSTEM_PROMPT="" both omit it; otherwise the configured or
 * built-in prompt is combined with agent_env_build_suffix(model). */
char *build_system_prompt(const char *model, int raw);

/* Per-run state shared by interactive and one-shot paths. */
struct agent_session {
    /* Config strings are copied because runtime /provider, /model, and /effort
     * commits can replace and free the config tier that supplied them. */
    char *model;               /* owned; NULL/"" = no model resolved yet */
    char *reasoning_effort;    /* owned; NULL = "omit" */
    const char *provider_name; /* borrowed from the live provider (valid for
                                * its lifetime), for stamping reasoning
                                * items. NULL = none. */
    char *sys;                 /* owned; NULL = no system message */
    struct tool_def *tools;    /* owned; NULL when n_tools == 0 */
    size_t n_tools;
    /* Preserve --raw across reconfigure so model/provider switches do not
     * reintroduce system text or tools. */
    int raw;

    struct item *items;
    size_t n_items;
    size_t cap_items;
};

/* Resolve per-request settings. Missing model is allowed so the REPL can start
 * and let the user choose one; non-interactive callers must reject it. */
int agent_session_init(struct agent_session *s, struct provider *p, const struct hax_opts *opts);

/* Re-resolve model/effort and rebuild the prompt after a provider/model switch;
 * history and the tools table are left intact. */
int agent_session_reconfigure(struct agent_session *s, struct provider *p);

void agent_session_free(struct agent_session *s);

/* Drop conversation items but keep session-level config and vector capacity. */
void agent_session_reset(struct agent_session *s);

/* Borrow a provider-facing context from the session until its next mutation. */
struct context agent_session_context(const struct agent_session *s);

/* Append a user message preceded by a turn boundary. */
void agent_session_add_user(struct agent_session *s, const char *text);

/* Append a boundary between provider round-trips inside one user turn. */
void agent_session_add_boundary(struct agent_session *s);

/* Drain successful turn items into the session. Reports the pre-drain item count
 * and whether a newly-drained item is a tool call. Does not reset `t`. */
void agent_session_absorb(struct agent_session *s, struct turn *t, size_t *out_before,
                          int *out_had_tool_call);

#endif /* HAX_AGENT_CORE_H */
