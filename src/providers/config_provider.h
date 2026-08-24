/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CONFIG_PROVIDER_H
#define HAX_PROVIDERS_CONFIG_PROVIDER_H

#include <jansson.h>
#include <stddef.h>

#include "provider.h"
#include "providers/registry.h"
#include "providers/wire.h"

/* Generic, data-driven provider construction.
 *
 * Every field of a provider_def resolves as the providers.<id> config block overlaid on the
 * def's defaults, so one constructor serves shipped and user-defined providers alike. The API
 * key is the one value read from the environment, via a def- or config-declared api_key_env:
 * a secret belongs in the environment, not the config file. */

/* Build the provider described by `def` from its config-overlaid data and capability hooks.
 * Returns NULL after reporting a user-actionable error. Construct overrides bypass this;
 * callers normally go through provider_construct in registry.h instead. */
struct provider *provider_def_construct(const struct provider_def *def);

/* Availability for the /provider picker. A keyed (cloud) provider — one with a declared
 * api_key_env or an inline api_key — is selectable iff that key resolves, with no network
 * probe (fast, and a 401 would be the only extra signal). A keyless one counts its configured
 * base_url as availability, except for defs that opt into a reachability probe. */
void provider_def_availability(const struct provider_def *def, struct provider_availability *out);

/* Field vocabulary of a providers.<name> block. The inventory is declarative only: value
 * acceptance lives in the dialect constructors, and the env-alias rows in config.c project a
 * subset of it (a unit test keeps them in sync). */
enum provider_field_dialect {
    PROVIDER_FIELD_OPENAI_CHAT = 1 << 0,      /* openai-completions */
    PROVIDER_FIELD_OPENAI_RESPONSES = 1 << 1, /* openai-responses */
    PROVIDER_FIELD_ANTHROPIC = 1 << 2,        /* anthropic-messages */
    /* Consumed by the generic constructor only when the def is unpinned: a pinned provider's
     * identity fields are fixed, so setting them must warn rather than silently do nothing. */
    PROVIDER_FIELD_UNPINNED = 1 << 3,
};
#define PROVIDER_FIELD_OPENAI (PROVIDER_FIELD_OPENAI_CHAT | PROVIDER_FIELD_OPENAI_RESPONSES)

struct provider_field {
    const char *leaf;
    unsigned dialects;   /* mask of enum provider_field_dialect */
    unsigned secret : 1; /* value must never be displayed */
};

/* The full inventory; *n receives its length. */
const struct provider_field *provider_fields(size_t *n);

/* Warn about providers.<name> members nothing consumes. A member is consumed by `wire`'s
 * dialect fields (NULL: none), by `extra_dialects`, by being a registered per-provider
 * setting, or by the NULL-terminated `extra` allowlist (may be NULL). Warnings never fail
 * construction, so a config written for a newer hax still runs. */
void provider_warn_unused_fields(const char *name, const struct wire *wire, unsigned extra_dialects,
                                 const char *const *extra);

/* Wire-preset variant: resolves providers.<name>.api the way construction does — moving an
 * OpenAI-family `default_wire` between the two OpenAI protocols, never across families — and
 * warns against the resulting dialect. Construction owns warning about the value itself. */
void provider_warn_unused_wire_fields(const char *name, const struct wire *default_wire,
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
