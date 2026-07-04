/* SPDX-License-Identifier: MIT */
#ifndef HAX_SELECT_H
#define HAX_SELECT_H

struct agent_state;

/*
 * Interactive runtime selection of provider, model, and reasoning effort,
 * built on the generic picker (terminal/picker.h). Each flow persists the
 * choice to the machine-local state tier (state.json, via
 * config_persist_state) and applies it to the live session
 * (agent_apply_settings), so a pick takes effect on the next turn and
 * sticks across runs without touching dotfiles-managed config.
 *
 * The flows chain in the order the conversation cares about:
 *
 *   select_provider — list every known provider alphabetically
 *     (unconfigured / unreachable ones dim with a reason, but still
 *     selectable: picking one re-checks availability and reports the exact
 *     reason if it's still unusable), switch to the chosen one, then chain
 *     into model and effort selection. The provider switch clears the prior
 *     model/effort (they belong to the old backend).
 *   select_model    — list the current provider's models, then chain into
 *     effort selection.
 *   select_effort   — list the current provider's reasoning-effort levels
 *     (plus a "default" that defers to the provider).
 *
 * All require an interactive terminal; on a non-tty the underlying picker
 * returns immediately and the flow is a no-op. These drive the render
 * pipeline themselves, so the slash commands that call them are registered
 * drives_disp.
 */
void select_provider(struct agent_state *st);
void select_model(struct agent_state *st);
void select_effort(struct agent_state *st);

struct provider;

/* Cold-start auto-pick used when the built-in default provider can't be
 * constructed and the user hasn't explicitly chosen one. Probes every
 * backend and returns the first available, constructed provider (noting
 * which on stdout), or NULL when none is available. The choice is not
 * persisted — it's re-evaluated each start, so the real default returns
 * once it's usable again. */
struct provider *provider_autoselect(void);

#endif /* HAX_SELECT_H */
