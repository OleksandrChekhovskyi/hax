/* SPDX-License-Identifier: MIT */
#ifndef HAX_SELECT_H
#define HAX_SELECT_H

struct agent_state;
struct provider;
struct model_info;
struct catalog_entry;
struct completion;

/* Selectors persist a choice and apply it to the live session. A NULL argument opens the pickers,
 * which chain from provider to model to effort. An argument applies directly without a picker: a
 * provider switches with its default or discovered model, if any, and default effort; a model keeps
 * the requested effort as far as the model offers it, and a level must be one the live model offers
 * or "default". */
void select_provider(struct agent_state *state, const char *provider);
void select_model(struct agent_state *state, const char *model);
void select_effort(struct agent_state *state, const char *level);

/* Add the arguments the selectors accept, for Tab completion, without waiting on the network or
 * background work: provider ids, sorted; the live provider's last-listed model ids, in model picker
 * order and split into parts at slashes; and the live model's effort levels followed by
 * "default". */
void select_provider_choices(struct completion *choices);
void select_model_choices(struct agent_state *state, struct completion *choices);
void select_effort_choices(struct agent_state *state, struct completion *choices);

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

/* Add the arguments /config and /preset-save take, for Tab completion: the setting keys the
 * /config picker lists, in its order and split into parts at dots; the values an editable `key`
 * accepts followed by "default", or none for any other key; and the tints a preset can carry. */
void select_config_key_choices(struct completion *choices);
void select_config_value_choices(const char *key, struct completion *choices);
void select_tint_choices(struct completion *choices);

/* Build an owned model-picker description. `model` is required; `configured` and `catalog` may be
 * NULL and merge around it with model_meta_merge precedence. Unknown fields are omitted. Returns
 * NULL when there is nothing to describe. */
char *model_desc_line(const struct model_info *model, const struct catalog_entry *configured,
                      const struct catalog_entry *catalog);

/* Return the first available provider in autoselect priority order, newly constructed, or NULL. */
struct provider *provider_autoselect(void);

#endif /* HAX_SELECT_H */
