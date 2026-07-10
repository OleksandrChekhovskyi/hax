/* SPDX-License-Identifier: MIT */
#ifndef HAX_CONFIG_H
#define HAX_CONFIG_H

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
 *   runtime override  →  environment  →  state  →  config file
 *                                                →  registry default
 *
 *   - override: set this session via config_set_override (e.g. a /model
 *     slash command); never persisted unless also written via config_persist
 *     or config_persist_state.
 *   - environment: the HAX_* var, verbatim including an empty string, so the
 *     quick `HAX_FOO=bar hax ...` invocation always wins.
 *   - state: ~/.local/state/hax/state.json — the machine-local,
 *     *persisted* layer of the override tier (what /provider, /model, /effort
 *     write). It holds only canonical config keys, sits in the XDG state dir
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
 * Two keys resolve with one extra rule: "model" and "reasoning_effort" are
 * provider-bound. They only mean something relative to a provider, so the
 * selectors persist them as one set with the "provider" they were picked for
 * (config_persist_selection), and a config file pairing them with a provider
 * reads the same way. At the state and file tiers a bound value applies only
 * while that tier's recorded provider is the active one; otherwise the tier
 * is skipped for these keys and resolution falls through — a one-off
 * HAX_PROVIDER=mock doesn't inherit a model saved for codex. A tier that
 * records no provider is unbound and applies as-is, and the env/override
 * tiers always apply: they are explicit for this run/session.
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

/* Resolved string for `key` (override → env → file → registry default), or
 * NULL when unset everywhere. Borrowed. Values are returned verbatim
 * including "" — empty is meaningful for some settings (HAX_SYSTEM_PROMPT=""
 * sends no system prompt, HAX_REASONING_EFFORT="" omits it). */
const char *config_str(const char *key);

/* Like config_str, but an empty value at any tier counts as unset there and
 * resolution falls through to the next tier. For string settings where ""
 * has no meaning (e.g. ports): a stray HAX_FOO= must not shadow a configured
 * value or end up interpolated into a URL. */
const char *config_str_nonempty(const char *key);

/* The registry default for `key` (the last resolution tier), or NULL when
 * the setting has none (or the key is unknown). For call sites whose
 * validity rules are stricter than the shared grammar (e.g. a retry base
 * must be > 0): a semantically invalid value falls back to this without
 * re-stating the constant at the site. */
const char *config_default(const char *key);

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

/* Set a session-only override for `key` (highest priority); val == NULL
 * clears it. Not persisted. The seam runtime selection (/model, /provider,
 * /effort, /preset) writes to. */
void config_set_override(const char *key, const char *val);

/* Snapshot / restore the whole override tier, for trying an operation that may
 * mutate overrides as a side effect and rolling it back on abort — e.g.
 * constructing a prospective provider (some set an override during probing)
 * before the user has committed to the switch. `snapshot` returns an opaque
 * owned handle; `restore` consumes it, replacing the live override tier (so on
 * the success path, discard the handle with config_override_state_free instead
 * of restoring). */
struct config_override_state;
struct config_override_state *config_override_snapshot(void);
void config_override_restore(struct config_override_state *snap);
void config_override_state_free(struct config_override_state *snap);

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
 * provider's default", pass CONFIG_VALUE_DEFAULT itself. Returns 0 on
 * success, -1 on I/O failure (the in-memory tier is then left unchanged). */
int config_persist_selection(const char *provider, const char *model, const char *effort);

/* ---- introspection ---- */

/* One row of the setting registry: canonical key, its env-var binding, the
 * fixed default (NULL when dynamic/computed/per-provider), and a one-line
 * human description. */
struct config_setting {
    const char *key;
    const char *env;
    const char *def;
    const char *desc;
};

/* The setting registry as a read-only array; *n receives the count. The
 * single source of truth for "what settings exist" — for generating a help
 * listing or a /config view, and for keeping docs honest. */
const struct config_setting *config_settings(size_t *n);

#endif /* HAX_CONFIG_H */
