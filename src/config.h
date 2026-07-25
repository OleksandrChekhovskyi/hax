/* SPDX-License-Identifier: MIT */
#ifndef HAX_CONFIG_H
#define HAX_CONFIG_H

#include <jansson.h>
#include <stddef.h>

/*
 * Process-wide configuration.
 *
 * One uniform system for every tunable. Each setting has a canonical,
 * provider-agnostic key (e.g. "model", "openai.base_url", "http.retry_base")
 * and an environment-variable binding (HAX_MODEL, HAX_OPENAI_BASE_URL, ...)
 * declared once in a registry inside config.c. Callers read settings by
 * canonical key — never getenv — so a setting can't be accidentally
 * env-only, and so the file, presets, runtime overrides, and a future
 * /config view all speak the same names; the env var is just one binding.
 *
 * Resolution, highest priority first:
 *
 *   run override  →  conversation  →  environment  →  state  →  config file
 *                                                            →  registry default
 *
 *   - run override: set for this run via config_set_override (a CLI
 *     selection flag, an explicit --preset, a /model slash command); never
 *     persisted unless also written via config_persist or
 *     config_persist_state.
 *   - conversation: the selection a resumed session recorded (see
 *     config_set_conversation). It is the payload of an explicit `--resume` /
 *     `-c` / `/resume`, so it outranks configuration — resuming a
 *     conversation continues it on the backend it was using, and the
 *     selection flags are how you say otherwise. Empty when nothing was
 *     resumed.
 *   - environment: the HAX_* var, so a `HAX_FOO=bar hax ...` invocation wins
 *     over everything persisted. An empty value is skipped (resolution falls
 *     through to a lower tier) for most settings; it's honored verbatim only
 *     for the keep_empty few where "" carries meaning — see config_str.
 *   - state: ~/.local/state/hax/state.json — the machine-local,
 *     *persisted* layer of the run-override tier (what /provider, /model,
 *     /effort write). It holds only canonical config keys, sits in the XDG state dir
 *     (not the config dir) so it stays out of a dotfiles repo, and overrides
 *     the committed config file while still yielding to an explicit env var
 *     for a one-off invocation. Same nested/flat key grammar as the config
 *     file. Genuinely different-shaped state (prompt history, sessions,
 *     future auth tokens) lives in its own files, not here.
 *   - config file: ~/.config/hax/config.json (the same dir as AGENTS.md /
 *     skills) — the declared, committable defaults. Nested objects are the
 *     friendly form — {"openai": {"base_url": "..."}} — and a flat dotted key
 *     ({"openai.base_url": "..."}) is also accepted. Plain JSON (jansson
 *     parses no comments or trailing commas); scalar values are read as
 *     strings, so 5000 and "5000" are equivalent.
 *   - registry default: a fixed fallback declared with the setting, so a
 *     shared default lives in exactly one place. Settings whose default is
 *     dynamic (model → the provider's), computed (notify → terminal
 *     detection), or per-provider (openai.base_url) carry no registry
 *     default and their consumer supplies it when config_str returns NULL.
 *
 * Two keys resolve with one extra rule: "model" and "effort" are
 * provider-bound. They only mean something relative to a provider, so the
 * selectors persist them as one set with the "provider" they were picked for
 * (config_persist_selection), a config file pairing them with a provider
 * reads the same way, and a resumed conversation records the set it was last
 * using. At the conversation, state, and file tiers a bound value applies
 * only while that tier's recorded provider is the active one; otherwise the
 * tier is skipped for these keys and resolution falls through — a one-off
 * HAX_PROVIDER=mock doesn't inherit a model saved for codex, and
 * `--provider=x --resume=ID` doesn't run a resumed conversation's model
 * against a different backend. A tier that records no provider is unbound and
 * applies as-is, and the env/run-override tiers always apply: they are
 * explicit for this run.
 *
 * The whole system is optional: with no file and no overrides, every lookup
 * is just getenv-or-default, so an env-vars-only setup is unchanged.
 *
 * Strings returned by config_str are borrowed and valid until config_free /
 * the next config_load / a config_set_override of the same key — i.e. for
 * the whole run in normal use.
 */

/* Sentinel value meaning "explicitly use the default". When a tier resolves
 * to this, config_str returns the registry default when the key declares one,
 * else NULL — so the consumer falls to its own (or the provider's) default —
 * AND resolution stops there: it does NOT fall through to lower tiers. It's
 * the third state beyond "absent" (fall through to the next tier) and "a
 * value" (use it verbatim). The runtime selectors write it
 * for a "use the provider's default" pick (the effort picker's "default" row,
 * or a provider switch that keeps no model), so the choice shadows a stale
 * lower-tier value (env/config) instead of letting it resurface under the new
 * provider. (An env var still wins on a fresh launch, per the order above —
 * the sentinel sits in the override/state tiers, not above env.) */
#define CONFIG_VALUE_DEFAULT "(default)"

/* ---- lifecycle ---- */

/* Load the config-file tier from JSON text, replacing any prior file tier.
 * Returns 0 for NULL/empty (cleared) or a JSON object, -1 for malformed or
 * non-object input (tier left empty). Scalar leaves are normalized to
 * strings. The pure seam config_init() builds on, and what tests drive. */
int config_load(const char *text);

/* Like config_load, but for the state tier (state.json). Same grammar and
 * return contract; the pure seam config_init() builds on for the state file,
 * and what tests drive. */
int config_load_state(const char *text);

/* Read ~/.config/hax/config.json into the file tier. Absent file is silent
 * (config is optional); a present-but-unusable file (malformed, non-object,
 * oversized) is ignored with a warning. Call once at startup, before any
 * setting is read. */
void config_init(void);

/* Release the file tier and any overrides. Optional — for clean shutdown /
 * ASan; a no-op when nothing is loaded. */
void config_free(void);

/* ---- read (by canonical key) ---- */

/* Resolved value for `key`, or NULL when unset. Borrowed. Empty values are
 * skipped unless the registry marks them meaningful; unknown keys preserve
 * them. Use config_str_nonempty for unknown keys that must skip empty values. */
const char *config_str(const char *key);

/* Like config_str, but always skips empty tier values. Intended for dynamic
 * keys absent from the registry; prefer config_str for registered settings. */
const char *config_str_nonempty(const char *key);

/* The registry default for `key` (the last resolution tier), or NULL when
 * the setting has none (or the key is unknown). For call sites whose
 * validity rules are stricter than the shared grammar (e.g. a retry base
 * must be > 0): a semantically invalid value falls back to this without
 * re-stating the constant at the site. */
const char *config_default(const char *key);

/* The JSON node at nested `key` — state tier first, then the config file
 * (highest tier that defines the block wins; no cross-tier merge). For
 * structured blocks whose member keys can themselves contain dots
 * (catalog.models.<provider>.<model-id> — model ids like "llama-3.2" would
 * split wrong under the flat dotted-key grammar), so the caller walks the
 * object with jansson directly. Borrowed; valid until config_free / the next
 * config_load*. Scalar leaves follow the load-time normalization: numbers
 * and booleans read as strings. NULL when absent. */
const json_t *config_json_node(const char *key);

/* Enumerate the immediate member names of the JSON object at nested key
 * `key` (e.g. "providers"), merged and deduplicated across the file and
 * state tiers. Returns the count; *out receives a freshly-allocated array of
 * `count` heap-owned strings (caller frees each element, then the array;
 * NULL with count 0 when the object is absent). The basis for config-defined
 * providers: each providers.<name> block is one selectable provider. */
size_t config_object_keys(const char *key, char ***out);

/* Typed views over the same resolution, centralizing the parse so every
 * setting shares one grammar. They resolve like config_str_nonempty (the
 * grammars give "" no meaning), and a value that fails to parse falls back
 * to the registry default (a typo'd timeout must not read as "disabled");
 * with no registry default either, the type-zero (0 / 0 / false) is
 * returned. Note parse_size treats 0 as invalid, so an explicit "0" also
 * reads as the default for config_size — but is honored by
 * config_duration_ms, where "0 disables" is part of the grammar. Same for
 * negative values via config_int: every int setting is a count or width,
 * so they read as invalid (add a signed variant if that ever changes). */
int config_int(const char *key);
int config_bool(const char *key);         /* 1/true/yes/on vs 0/false/no/off (case-insensitive) */
long config_size(const char *key);        /* parse_size grammar: 4096, 64k, 1M */
long config_duration_ms(const char *key); /* parse_duration_ms grammar: 500, 2s */

/* config_bool for settings whose default lives at the single call site,
 * not in the registry (openai.send_cache_key's per-preset choice): unset,
 * empty, or unrecognized values read as `def` — so a typo'd value never
 * flips the switch away from the caller-supplied default. */
int config_bool_or(const char *key, int def);

/* ---- write ---- */

/* Which tier a write lands in. Only the two caller-selectable ones: the
 * lower tiers are file-backed and written through config_persist*. */
enum config_tier {
    CONFIG_TIER_RUN = 0,      /* config_set_override */
    CONFIG_TIER_CONVERSATION, /* config_set_conversation */
};

/* Set a run-scoped override for `key` (highest priority); val == NULL
 * clears it. Not persisted. The seam runtime selection (/model, /provider,
 * /effort, /preset) and the CLI selection flags write to. */
void config_set_override(const char *key, const char *val);

/* Set `key` in the conversation tier (below run overrides and above env);
 * val == NULL clears just that key. Never persisted: it describes the
 * conversation being resumed, not a new default, so an explicit pick made
 * during the run writes a run override (and state.json) as usual and
 * shadows this. Write "provider" alongside "model"/"effort" so the
 * provider-binding rule above can skip a stale pair when the active
 * provider differs. */
void config_set_conversation(const char *key, const char *val);

/* Drop the whole conversation tier (nothing resumed / the restore was
 * rejected), so resolution falls back to env, state, and file. */
void config_clear_conversation(void);

/* Exit any preset stance in `tier`: clears the stance name (empty, not
 * absent, at the run tier — the banner must never claim a stance that isn't
 * applied) and the preset-owned system prompt. At CONFIG_TIER_RUN it also
 * drops a stance a resumed conversation restored into the tier below,
 * because clearing a run override only unhides the lower tier: an explicit
 * pick would otherwise keep running under the resumed preset's system prompt
 * while reporting no stance. What an explicit /provider, /model, or /effort
 * commit calls; the preset writers below call it for themselves. */
void config_preset_exit(enum config_tier tier);

/* Write a selection a session recorded into `tier` — the one definition of
 * what "resume this conversation's setup" means, shared by the two paths that
 * restore one: `--resume` / `-c` at startup writes CONFIG_TIER_CONVERSATION
 * (a restore the run's own flags still outrank), and `/resume` mid-session
 * writes CONFIG_TIER_RUN (the newest explicit act, so it outranks a pick made
 * earlier in the run).
 *
 * `provider` anchors it: absent, empty, or the "none" a provider-less
 * recording carries means there is nothing to go back to, and only the stance
 * below is pinned. A recorded model/effort is pinned as the pair it was used
 * as; a recorded *absence* pins the sentinel, so the conversation keeps
 * running on the provider's own default instead of picking up a value saved
 * for it in state.json.
 *
 * `preset` restores the stance whole (system prompt included), replacing the
 * individual values above — by name, so an edited definition applies as
 * edited. Pass NULL when the conversation recorded none, or when the caller
 * makes an explicit selection of its own: a preset is all-or-nothing, so
 * naming one member can only exit the stance (exactly as an explicit
 * /provider, /model, or /effort pick exits a live one), never half-keep it.
 * Either way the tier pins an empty stance and drops any system prompt the
 * outgoing one set, so no preset can claim a conversation that didn't run
 * under it.
 *
 * Returns 0, or -1 when a recorded preset no longer applies (renamed,
 * deleted, made invalid) with *err set to a malloc'd reason — the caller
 * decides whether that's fatal. The rest of the selection is restored either
 * way, so an interactive caller can carry on without the stance. */
int config_restore_selection(enum config_tier tier, const char *provider, const char *model,
                             const char *effort, const char *preset, char **err);

/* Snapshot / restore both caller-writable tiers (run overrides and the
 * conversation tier), for trying an operation that may mutate them as a side
 * effect and rolling it back on abort — e.g. constructing a prospective
 * provider (some set an override during probing) before the user has
 * committed to the switch. Both, not just the overrides: a run-tier stance
 * change reaches into the conversation tier too (config_preset_exit), so
 * restoring one without the other would leave a rejected switch half-applied.
 * `snapshot` returns an opaque owned handle; `restore` consumes it, replacing
 * the live tiers (so on the success path, discard the handle with
 * config_snapshot_free instead of restoring). */
struct config_snapshot;
struct config_snapshot *config_snapshot_take(void);
void config_snapshot_restore(struct config_snapshot *snap);
void config_snapshot_free(struct config_snapshot *snap);

/* Persist `key` = `val` into ~/.config/hax/config.json (nested form),
 * preserving the file's other keys; val == NULL removes the key. Atomic
 * (temp + rename, 0600). Updates the in-memory file tier too, so the new
 * value is visible immediately. Returns 0 on success, -1 on I/O failure.
 * The "remember this setting" seam for the committed config file. */
int config_persist(const char *key, const char *val);

/* Like config_persist, but writes the machine-local state tier
 * (~/.local/state/hax/state.json) instead of the committed config file.
 * The "remember my runtime pick" seam — what /provider, /model, /effort
 * call so a selection sticks across runs without touching dotfiles-managed
 * config. Pair with config_set_override for immediate same-session effect. */
int config_persist_state(const char *key, const char *val);

/* Persist a provider/model/effort selection into the state tier as one
 * atomic write — the seam the runtime selectors commit through, keeping the
 * set coherent for the provider-binding resolution rule above. `provider`
 * anchors the selection and is required. A NULL model/effort means "not
 * picked this time": the stored value is kept when `provider` matches the
 * recorded one (an /effort pick must not wipe a saved model) and reset to
 * CONFIG_VALUE_DEFAULT when the selection re-pins a different provider (the
 * old value was picked for the old provider). To write "explicitly use the
 * provider's default", pass CONFIG_VALUE_DEFAULT itself. Also removes any
 * persisted preset name in the same write: an explicit selection commit
 * replaces the preset stance (see config_preset_apply). Returns 0 on
 * success, -1 on I/O failure (the in-memory tier is then left unchanged). */
int config_persist_selection(const char *provider, const char *model, const char *effort);

/* ---- presets ---- */

/* Apply the preset named `name` — a full selection commit from the config's
 * presets.<name> object, written to `tier`. CONFIG_TIER_RUN is an explicit
 * apply (a --preset flag, /preset, a configured stance): above env, below
 * the explicit CLI selection flags, which the caller applies afterwards.
 * CONFIG_TIER_CONVERSATION restores the stance a resumed session recorded,
 * so a user's env var and flags both still win. The members: "provider"
 * (required) anchors the selection;
 * "model" and "effort" are optional and reset to
 * CONFIG_VALUE_DEFAULT when unnamed, so the provider's own default applies;
 * "system_prompt" is optional and cleared when unnamed, so normal
 * resolution returns. Because every apply writes the whole set, presets
 * replace each other rather than compose. No other keys are presettable
 * (see PRESET_KEYS in config.c for why); "description" is reserved
 * metadata for the /preset picker and the system prompt's preset listing.
 * Validation is all-or-nothing: any invalid member applies nothing.
 * A successful apply also records `name` under the "preset" key in the same
 * tier — the active-stance signal the banners and /session read; an explicit
 * /provider, /model, or /effort commit clears it (the pick exits the
 * stance). Returns 0 on success; -1 on failure with *err (when non-NULL)
 * set to a malloc'd human-readable reason the caller prints and frees. */
int config_preset_apply(const char *name, enum config_tier tier, char **err);

/* The "description" member of presets.<name>, or NULL when the preset or
 * the member is absent. Borrowed; valid until config_free / config_load*. */
const char *config_preset_description(const char *name);

/* The "provider" member of presets.<name>, or NULL when the preset is
 * absent (validation guarantees it's present and non-empty for every
 * enumerated name). For consumers that check the name resolves against the
 * provider registry — a check that lives above this layer, since config
 * must not depend on provider discovery. Borrowed, same lifetime as
 * config_preset_description. */
const char *config_preset_provider(const char *name);

/* Enumerate the defined presets: the member names of the presets object,
 * merged and deduplicated across the state and file tiers, restricted to
 * names that resolve to a *structurally valid* preset — the same
 * validation apply runs — so everything listed is guaranteed to apply.
 * Unusable definitions (fully-flat leaf spellings, a missing provider, an
 * unknown member) are skipped with a once-per-process warning: they are
 * user-authored config that isn't being honored. (A definition must be an
 * object: nested under "presets", or a single flat "presets.<name>" key —
 * the same exception to the flat-key grammar that structured blocks like
 * catalog.models carry.) Same ownership contract as config_object_keys:
 * caller frees each element, then the array. */
size_t config_preset_names(char ***out);

/* ---- introspection ---- */

/* Grammar for settings without enumerated choices. CFG_STRING accepts any
 * non-NULL value; numeric kinds mirror the typed getters. */
enum config_kind {
    CFG_STRING = 0,
    CFG_INT,      /* whole number >= 0 (parse_int / config_int) */
    CFG_SIZE,     /* parse_size grammar: 4096, 64k, 1M (> 0) */
    CFG_DURATION, /* parse_duration_ms grammar: 500, 2s (>= 0; 0 disables) */
};

/* Two choice strings carry special grammar in config_value_valid and in the
 * /config display, so the registry, validator, and display reference them by
 * name rather than repeating the literal. CONFIG_CHOICES_BOOL accepts the full
 * boolean grammar (on/off/1/0/true/false/…); CONFIG_CHOICES_TRISTATE adds an
 * "auto" literal on top, for a boolean whose default the consumer resolves
 * itself (a provider preset), so unset/auto means "let it decide". */
#define CONFIG_CHOICES_BOOL     "on|off"
#define CONFIG_CHOICES_TRISTATE "auto|on|off"

/* One setting registry row. */
struct config_setting {
    const char *key;
    const char *env;
    const char *def;
    const char *desc;
    const char *choices;     /* '|'-separated enum; see CONFIG_CHOICES_* above */
    enum config_kind kind;   /* grammar when choices is NULL */
    long min;                /* lower bound in int/bytes/ms; 0 = none */
    long max;                /* upper bound in int/bytes/ms; 0 = none */
    unsigned runtime : 1;    /* editable through /config */
    unsigned secret : 1;     /* redact the value in /config */
    unsigned keep_empty : 1; /* "" is meaningful rather than unset */
};

/* The setting registry as a read-only array; *n receives the count. The
 * single source of truth for "what settings exist" — for generating a help
 * listing or a /config view, and for keeping docs honest. */
const struct config_setting *config_settings(size_t *n);

/* Registry row for `key`, or NULL if unknown. */
const struct config_setting *config_setting_find(const char *key);

/* Winning tier: "session", "env", "state", "config", or "default". Uses
 * the same empty-value policy as config_str. */
const char *config_source(const char *key);

/* Validate `val` against case-insensitive choices (with the full boolean
 * grammar for "on|off"), or against the setting's kind and bounds. */
int config_value_valid(const struct config_setting *s, const char *val);

/* Format choices or the kind grammar and bounds for a rejection message.
 * Empty for an unrestricted CFG_STRING. */
void config_value_hint(const struct config_setting *s, char *buf, size_t n);

/* Return the heap-owned canonical enum spelling matching `val`, or NULL for
 * a non-enum or no match. Validate `val` first; the caller frees the result. */
char *config_value_canonical(const struct config_setting *s, const char *val);

#endif /* HAX_CONFIG_H */
