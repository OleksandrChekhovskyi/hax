/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CONFIG_PROVIDER_H
#define HAX_PROVIDERS_CONFIG_PROVIDER_H

#include <jansson.h>
#include <stddef.h>

#include "provider.h"
#include "providers/openai.h"

/* Providers defined by data rather than code.
 *
 * A config-defined provider is a named `providers.<name>` block in config.json, optionally
 * seeded by a built-in recipe of defaults (see RECIPES in config_provider.c — e.g. "ollama").
 * Each speaks one dialect — "openai-completions" or "anthropic-messages" — and resolves its
 * settings from its own subtree overlaid on the recipe. The API key is the one value read
 * from the environment, via a recipe- or config-declared api_key_env: a secret belongs in
 * the environment, not the config file. */

/* The dynamic factory set: the union of recipe names and config.json `providers.*` names,
 * deduplicated — a config block matching a recipe name overlays that recipe rather than
 * adding a second entry. Heap-built once on first call and cached for the process; *n
 * receives the count. Names are not filtered against the compiled-in factories; the
 * registry does that when merging. */
const struct provider_factory *const *config_providers(size_t *n);

/* Field vocabulary of a providers.<name> block. The inventory is declarative only: value
 * acceptance lives in the dialect constructors, and the env-alias rows in config.c project a
 * subset of it (a unit test keeps them in sync). Construction warns about block members
 * outside the provider's dialect. */
enum provider_field_dialect {
    PROVIDER_FIELD_OPENAI_CHAT = 1 << 0,      /* openai-completions */
    PROVIDER_FIELD_OPENAI_RESPONSES = 1 << 1, /* openai-responses */
    PROVIDER_FIELD_ANTHROPIC = 1 << 2,        /* anthropic-messages */
    /* Resolved by the config-provider machinery itself; compiled-in providers ignore it. */
    PROVIDER_FIELD_CONFIG_DEFINED = 1 << 3,
};
#define PROVIDER_FIELD_OPENAI (PROVIDER_FIELD_OPENAI_CHAT | PROVIDER_FIELD_OPENAI_RESPONSES)

struct provider_field {
    const char *leaf;
    unsigned dialects;   /* mask of enum provider_field_dialect */
    unsigned secret : 1; /* value must never be displayed */
};

/* The full inventory; *n receives its length. */
const struct provider_field *provider_fields(size_t *n);

/* Warn about providers.<name> block members the provider does not consume. A member is consumed
 * when its inventory dialect intersects `dialects`, when it is a registered per-provider setting
 * (a compiled-in module knob such as providers.llamacpp.port), or when the NULL-terminated
 * `extra` allowlist (may be NULL) names it. `api_label` names the accepting category in the
 * message. Warnings never fail construction, so a config written for a newer hax still runs. */
void provider_warn_unused_fields(const char *name, const char *api_label, unsigned dialects,
                                 const char *const *extra);

/* OpenAI-family variant: resolves providers.<name>.api against `default_wire` and derives the
 * dialect mask and label from the result. */
void provider_warn_unused_openai_fields(const char *name, enum openai_wire default_wire,
                                        const char *const *extra);

/* Resolve the provider's credential: inline <prefix>.api_key, else the named environment
 * variable. An inline "$NAME" value reads the environment variable NAME instead, keeping the
 * secret out of the config file ("$$" escapes a literal '$'); unresolved indirection falls
 * through to api_key_env. Borrowed; NULL when nothing resolves. */
const char *provider_api_key(const char *config_prefix, const char *api_key_env);

/* Resolve <prefix>.cache_ttl to a canonical static "5m" or "1h", warning on any other value.
 * Defaults to 1h, which suits an interactive agent's pauses better than the API's 5m. */
const char *provider_cache_ttl(const char *config_prefix);

/* Resolve <prefix>.extra_body: an owned object of raw JSON members a provider merges into each
 * request body it builds, or NULL. Protocol-owned members (model, messages, tools, ...) are
 * dropped with a warning, as is a non-object value. */
json_t *provider_extra_body(const char *config_prefix);

/* Merge extra-body members into `body` (NULL `extra_body` is a no-op). A member overrides the
 * built field of the same name, recursing where both sides are objects so a nested member
 * extends rather than replaces a built block. Merged values are shared with `extra_body`, so
 * the caller must not mutate `body` afterwards. */
void provider_extra_body_apply(json_t *body, const json_t *extra_body);

/* Resolve <prefix>.extra_headers, an object of header name/value members, into an owned
 * NULL-terminated array of "Name: value" strings for every request to the provider, or NULL.
 * A "$NAME" value reads the environment variable NAME, like an inline api_key. Invalid names
 * and values are dropped with a warning. */
char **provider_extra_headers(const char *config_prefix);

#endif /* HAX_PROVIDERS_CONFIG_PROVIDER_H */
