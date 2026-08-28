/* SPDX-License-Identifier: MIT */
#ifndef HAX_SELECT_H
#define HAX_SELECT_H

struct agent_state;
struct provider;
struct model_info;
struct catalog_entry;

/* Interactive selectors persist a choice and apply it to the live session. */
void select_provider(struct agent_state *state);
void select_model(struct agent_state *state);
void select_effort(struct agent_state *state);

/* Apply `name`, or open the preset picker when name is NULL. A fresh provider is constructed and
 * transferred only after validation succeeds. `announce` controls the confirmation display.
 * Returns 0 when applied and -1 on failure or cancellation. */
int select_preset(struct agent_state *state, const char *name, int announce);

/* Save "<name> [tint]" from the live selection and enter it; NULL seeds the command prompt. */
void select_preset_save(struct agent_state *state, const char *argument);

/* Restore a recorded selection without changing persisted defaults. Missing or unusable settings
 * are reported and leave the live provider intact. */
void select_restore_session(struct agent_state *state, const char *provider_id, const char *model,
                            const char *effort, const char *preset);

/* Open the settings picker, or apply "<key> [value]" as a run-scoped override. */
void select_config(struct agent_state *state, const char *argument);

/* Build an owned model-picker description. `model` is required; `configured` and `catalog` may be
 * NULL and merge around it with model_meta_merge precedence. Unknown fields are omitted. Returns
 * NULL when there is nothing to describe. */
char *model_desc_line(const struct model_info *model, const struct catalog_entry *configured,
                      const struct catalog_entry *catalog);

/* Return the first available provider in autoselect priority order, newly constructed, or NULL. */
struct provider *provider_autoselect(void);

#endif /* HAX_SELECT_H */
