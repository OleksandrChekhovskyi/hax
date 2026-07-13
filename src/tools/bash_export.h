/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_BASH_EXPORT_H
#define HAX_TOOLS_BASH_EXPORT_H

#include <stddef.h>

/*
 * The session's effective provider/model/effort, published by the agent
 * layer and exported into bash-tool children as HAX_PROVIDER / HAX_MODEL /
 * HAX_EFFORT — so a nested `hax -p` (subagent) inherits exactly
 * what the parent sends on the wire, including session-only state (an
 * auto-selected provider, a /model pick) that lives in the override tier,
 * not the environment. HAX_PRESET is cleared alongside: a preset the parent
 * ran under already shaped these values, and re-applying it in the child
 * would shadow them.
 *
 * Its own translation unit (not bash.c) so the agent core can publish
 * without linking the whole bash tool — unit tests stub TOOL_BASH.
 * Written and read on the dispatch thread only (a tool never runs
 * concurrently with a settings change), so no locking.
 */

/* Nesting cap for hax-in-hax. The bash tool stamps children with
 * HAX_SUBAGENT_DEPTH = parent + 1 (build_child_env) and main() refuses to
 * start at this depth — the backstop against a confused model recursively
 * spawning subagents. Lives here because the stamp and the guard must
 * agree on the value. */
#define HAX_SUBAGENT_MAX_DEPTH 3

/* Publish the selection. Called by the agent layer whenever the session's
 * model/effort are (re)resolved (agent_session_init / _reconfigure).
 * NULL/empty provider clears the export (children see the raw parent env).
 * Empty model/effort are meaningful exports, not omissions: an empty model
 * falls to the provider's default and an empty effort forces "send none" —
 * both exactly what the parent does when the value is unresolved — while
 * omitting them would let an unrelated var from the parent's own
 * environment leak through. */
void bash_export_selection(const char *provider, const char *model, const char *effort);

/* The published entries as "NAME=value" strings for the child env builder.
 * Returns the count (0 when nothing is published); *out borrows the array. */
size_t bash_export_env(const char *const **out);

#endif /* HAX_TOOLS_BASH_EXPORT_H */
