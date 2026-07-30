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

/* Switch to a preset (a presets.<name> selection from the config — see
 * config_preset_apply): by `name` when given, else via a picker over the
 * defined presets. Persists like the flows above, but by *name* (the
 * "preset" state key): the next launch re-applies the then-current
 * definition. An explicit /provider, /model, or /effort pick exits the
 * stance — commit_selection clears the name and the preset's system
 * prompt. The provider is always constructed fresh under the applied
 * overrides (construction runs value-dependent behavior like llama.cpp's
 * model reconciliation) and swapped in; a validation or construction
 * failure rolls the whole application back.
 *
 * `announce` = 0 suppresses the post-apply banner / "switched to …" line for
 * callers that print their own (`/new <preset>`). Returns 0 when a preset was
 * applied, -1 when nothing changed — a failure (diagnosed on screen) or a
 * cancelled picker. */
int select_preset(struct agent_state *st, const char *name, int announce);

/* Save the live selection as the preset named by `arg` ("<name> [tint]") and
 * switch into it, so the banner names the stance and the next launch starts in
 * it (config_preset_save writes the block). Provider, model, and effort come
 * from the session — except a model the backend discovered rather than the user
 * choosing it, which is left out so the preset re-discovers; a system prompt
 * only when normal resolution wouldn't bring it back; a re-save keeps the
 * existing description. Without the second word a picker asks for the tint,
 * where Escape abandons the save; with no `arg` at all, `/preset-save ` is
 * seeded back into the prompt. An existing name asks before it is replaced. */
void select_preset_save(struct agent_state *st, const char *arg);

/* Switch to the selection a resumed conversation recorded — the mid-session
 * twin of the restore `--resume` does at startup, so /resume continues a
 * conversation on the backend it was using rather than on whatever this run
 * happens to be set to. Applies as run overrides (the newest explicit act
 * wins over a /model made earlier in the run) and, unlike the flows above,
 * persists nothing: resuming states what this conversation used, not a new
 * default. A "none"/absent provider means the recording named no backend, so
 * the run keeps its own. Anything that can't be restored — a provider that
 * won't construct, no model for it, a preset since deleted — leaves the live
 * setup in place with a note: the history is back either way, and moving the
 * conversation to a different backend is the user's call. */
void select_restore_session(struct agent_state *st, const char *provider, const char *model,
                            const char *effort, const char *preset);

/* View or change configuration. Without `arg`, opens the picker; otherwise
 * accepts "<key> [value]". Changes are run-scoped overrides, and "default"
 * clears an override. */
void select_config(struct agent_state *st, const char *arg);

struct provider;
struct model_info;
struct catalog_entry;

/* Compose the /model picker's gutter for one model: context window, image
 * input, and per-Mtok rates on the first line, then the backend's own
 * one-line blurb (when it offered one) on the second. Tool support is
 * deliberately absent — it dims the row instead, being a verdict on whether
 * the model works here rather than a description of it.
 *
 * Layered through model_meta_merge: what the backend reported about this
 * model (`m`) wins field by field, and `cat` — the models.dev/config entry,
 * NULL when the model isn't in the catalog — fills the rest. Fields neither
 * source knows are omitted rather than rendered as zeros, so a bare-ids
 * backend with no catalog presence yields NULL (no gutter at all) instead
 * of a row of placeholders. Returns malloc'd; caller frees. Pure, for unit
 * tests. */
char *model_desc_line(const struct model_info *m, const struct catalog_entry *cat);

/* Cold-start auto-pick used when the built-in default provider can't be
 * constructed and the user hasn't explicitly chosen one. Probes every
 * backend and returns the first available, constructed provider (noting
 * which on stdout), or NULL when none is available. The choice is not
 * persisted — it's re-evaluated each start, so the real default returns
 * once it's usable again. */
struct provider *provider_autoselect(void);

#endif /* HAX_SELECT_H */
