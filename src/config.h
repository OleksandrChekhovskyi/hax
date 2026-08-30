/* SPDX-License-Identifier: MIT */
#ifndef HAX_CONFIG_H
#define HAX_CONFIG_H

#include <jansson.h>
#include <stddef.h>

/* Process-wide configuration, owned by the foreground thread. Settings resolve in this order:
 *
 *   run override -> resumed conversation -> environment -> state.json -> config.json -> default
 *
 * Callers use canonical keys rather than reading HAX_* variables directly. See
 * docs/configuration.md for user-facing file formats and resolution behavior.
 *
 * Returned strings and JSON nodes are borrowed. Copy values that must survive a config mutation or
 * environment change, and resolve all values before starting a background job. */

/* Stops resolution at the current tier. Resolves to the registry default, or NULL when the
 * consumer owns the default. */
#define CONFIG_VALUE_DEFAULT "(default)"

/* Load a JSON object into the config-file or state tier, replacing its previous contents. NULL,
 * empty, and whitespace-only input clear the tier. The tier keeps the document verbatim:
 * structured reads see the original JSON types, while string reads coerce scalar numbers and
 * booleans. Returns 0 on success, -1 for malformed JSON or a non-object root. */
int config_load(const char *text);
int config_load_state(const char *text);

/* Load config.json and state.json from their XDG paths. Missing files are ignored; unusable files
 * produce a warning. */
void config_init(void);
void config_free(void);

/* Resolve a canonical key. config_str preserves empty values only for settings that mark them
 * meaningful; config_str_nonempty always skips them. */
const char *config_str(const char *key);
const char *config_str_nonempty(const char *key);

/* Resolve a key without the run-override tier. */
const char *config_str_below_run(const char *key);

/* Return the registry default, or NULL for an unknown or dynamically defaulted setting. */
const char *config_default(const char *key);

/* Return a structured block from the state tier, then the config-file tier, verbatim with its
 * original JSON types. The first tier that defines the block wins; blocks are not merged. */
const json_t *config_json_node(const char *key);

/* Merge the immediate member names at `key` across the config-file and state tiers. `out` receives
 * an allocated array of allocated strings, or NULL when empty; the caller frees each string and
 * the array. */
size_t config_object_keys(const char *key, char ***out);

/* Typed lookups skip empty values and fall back to the registry default on parse or bounds errors.
 * With no valid default they return zero. Sizes must be positive; durations may be zero. */
int config_int(const char *key);
int config_bool(const char *key);
long config_size(const char *key);
long config_tokens(const char *key);
long config_duration_ms(const char *key);

/* Parsers for setting values, shared by the typed lookups and callers that hold an already-resolved
 * string. */
/* Parse positive byte counts with optional case-insensitive k/m binary suffixes; invalid input
 * yields 0. */
long parse_size(const char *str);
/* Parse positive token counts: same grammar, decimal suffixes (k = 1000, m = 1000000). */
long parse_token_count(const char *str);
/* Parse a non-negative duration with an optional ms/s/m/h suffix and return milliseconds. A missing
 * suffix means seconds. Returns -1 for invalid input. */
long parse_duration_ms(const char *str);

/* Parse a boolean setting, using `default_value` when it is unset or invalid. */
int config_bool_or(const char *key, int default_value);

/* Resolve "<prefix>.<leaf>" with the semantics of config_str, config_bool_or, and config_int.
 * A NULL prefix resolves nothing (NULL / fallback / 0): a caller without a config namespace
 * ships fixed behavior. */
const char *config_scoped_str(const char *prefix, const char *leaf);
int config_scoped_bool_or(const char *prefix, const char *leaf, int fallback);
int config_scoped_int(const char *prefix, const char *leaf);

/* Expand a non-NULL system_prompt/system_prompt_append value into malloc'd prompt text. A
 * leading '@' names a text file: `~` expands to $HOME and relative paths resolve against the
 * hax config directory. File contents are UTF-8 sanitized and trailing newlines are trimmed.
 * Other values are copied verbatim. Returns NULL when the file is unreadable or oversized,
 * storing an allocated message in `*error` when non-NULL. */
char *config_prompt_expand(const char *value, char **error);

enum config_tier {
    CONFIG_TIER_RUN = 0,
    CONFIG_TIER_CONVERSATION,
};

/* Set or clear (`value == NULL`) a value in a writable tier. */
void config_set_override(const char *key, const char *value);
void config_set_conversation(const char *key, const char *value);
void config_clear_conversation(void);

/* End the active preset in `tier`, including any preset-owned system prompt. A run-tier exit also
 * removes a preset restored in the conversation tier so it cannot resurface. */
void config_preset_exit(enum config_tier tier);

/* Restore recorded selection metadata into `tier`. Missing model/effort values select the provider
 * defaults. A provider of NULL, empty, or "none" restores only the preset stance. Returns -1 when
 * the recorded preset is no longer valid; the provider/model/effort remain restored. On failure,
 * `error` receives an allocated message when non-NULL. */
int config_restore_selection(enum config_tier tier, const char *provider, const char *model,
                             const char *effort, const char *preset, char **error);

/* Snapshot both writable tiers. Restore consumes the snapshot; free discards it. */
struct config_snapshot;
struct config_snapshot *config_snapshot_take(void);
void config_snapshot_restore(struct config_snapshot *snapshot);
void config_snapshot_free(struct config_snapshot *snapshot);

/* Atomically persist a nested key in config.json or state.json and update the in-memory tier.
 * `value == NULL` removes the key. The config-file write refuses to replace a file that could not
 * be loaded. Returns 0 on success, -1 on failure. */
int config_persist(const char *key, const char *value);
int config_persist_state(const char *key, const char *value);

/* Persist a provider-bound selection to state.json in one write. `provider` is required. A NULL
 * model or effort preserves the stored value for the same provider and selects the new provider's
 * default when the provider changes. Passing CONFIG_VALUE_DEFAULT explicitly always selects that
 * default. Returns 0 on success or -1 with the in-memory tier unchanged. */
int config_persist_selection(const char *provider, const char *model, const char *effort);

/* Apply a complete presets.<name> selection to `tier`. The preset must contain a provider; omitted
 * model and effort values select provider defaults. Returns -1 with an optional allocated `error`
 * message if the preset is absent or invalid. */
int config_preset_apply(const char *name, enum config_tier tier, char **error);

/* Borrowed preset members, or NULL when the preset or member is absent. */
const char *config_preset_description(const char *name);
const char *config_preset_tint(const char *name);
const char *config_preset_provider(const char *name);
const char *config_preset_model(const char *name);
const char *config_preset_effort(const char *name);

/* Return whether a definition exists, including an invalid or non-object definition. */
int config_preset_exists(const char *name);

/* Names contain 1-63 letters, digits, dots, hyphens, or underscores and start alphanumeric. */
int config_preset_name_valid(const char *name);

/* `provider` is required; NULL optional members are omitted when saving. */
struct config_preset {
    const char *description;
    const char *tint;
    const char *provider;
    const char *model;
    const char *effort;
    const char *system_prompt;
    const char *system_prompt_append;
};

/* Save a validated preset to config.json, replacing the same name and preserving other loaded
 * content. Returns -1 with an optional allocated `error` message on failure. */
int config_preset_save(const char *name, const struct config_preset *preset, char **error);

/* Enumerate valid, appliable preset names. Uses the config_object_keys ownership contract. */
size_t config_preset_names(char ***out);

enum config_kind {
    CONFIG_KIND_STRING = 0,
    CONFIG_KIND_INT,
    CONFIG_KIND_SIZE,   /* bytes; binary k/m suffixes */
    CONFIG_KIND_TOKENS, /* token counts; decimal k/m suffixes */
    CONFIG_KIND_DURATION,
};

/* Named choice grammars shared by registry validation and the /config UI. */
#define CONFIG_CHOICES_BOOL     "on|off"
#define CONFIG_CHOICES_TRISTATE "auto|on|off"

struct config_setting {
    const char *key;
    const char *env_var;
    const char *default_value;
    const char *description;
    const char *choices; /* '|'-separated; exhaustive for strings, additive for numeric kinds */
    const char *example; /* numeric example for a setting that also has symbolic choices */
    enum config_kind kind;
    long min;                /* native units (integer, bytes, or ms); 0 means no lower bound */
    long max;                /* native units (integer, bytes, or ms); 0 means no upper bound */
    unsigned editable : 1;   /* editable through /config */
    unsigned secret : 1;     /* value is redacted in /config */
    unsigned keep_empty : 1; /* an empty string is meaningful */
};

/* Registry rows are static and ordered for display. */
const struct config_setting *config_settings(size_t *count);
const struct config_setting *config_setting_find(const char *key);

/* Return "run", "conversation", "env", "state", "config", or "default". */
const char *config_source(const char *key);

/* Validate a value against the setting's choices, kind, and bounds. */
int config_value_valid(const struct config_setting *setting, const char *value);

/* Describe the accepted values for a validation error. Writes an empty string for unrestricted
 * string settings. */
void config_value_hint(const struct config_setting *setting, char *buffer, size_t size);

/* Return the allocated canonical enum choice matching `value`, or NULL for non-enums or no
 * match. */
char *config_value_canonical(const struct config_setting *setting, const char *value);

#endif /* HAX_CONFIG_H */
