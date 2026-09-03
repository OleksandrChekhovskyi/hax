/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_PROVIDER_CONFIG_H
#define HAX_PROVIDERS_PROVIDER_CONFIG_H

#include <jansson.h>
#include <stddef.h>

/* The providers.<name> config vocabulary: the field inventory, the unused-field lint, and
 * per-field resolution helpers, shared by the generic constructor (http_provider_new) and the
 * hook modules. The API key is the one value read from the environment, via a def- or
 * config-declared api_key_env: a secret belongs in the environment, not the config file. */

/* Field vocabulary of a providers.<name> block. The inventory is declarative only: value
 * acceptance lives in the dialect constructors, and the env-alias rows in config.c project a
 * subset of it (a unit test keeps them in sync).
 *
 * Each class names the providers that consume a field: the wire dialects for protocol knobs,
 * and def traits for fields tied to how the def authenticates or addresses its endpoint. */
enum provider_field_class {
    /* Wire dialects. */
    PROVIDER_FIELD_OPENAI_CHAT = 1 << 0,      /* openai-completions */
    PROVIDER_FIELD_OPENAI_RESPONSES = 1 << 1, /* openai-responses */
    PROVIDER_FIELD_ANTHROPIC = 1 << 2,        /* anthropic-messages */
    /* Def traits, added by construction when the def qualifies. */
    /* Unpinned defs: a pinned provider's identity fields are fixed, so setting them must warn
     * rather than silently do nothing. */
    PROVIDER_FIELD_UNPINNED = 1 << 3,
    /* Providers authenticating with a static API key; a def with an auth source manages its
     * own credentials, so key fields must warn. */
    PROVIDER_FIELD_KEYED = 1 << 4,
    /* Defs whose base_url carries a "{port}" placeholder. */
    PROVIDER_FIELD_PORT_TEMPLATED = 1 << 5,
};
#define PROVIDER_FIELD_OPENAI (PROVIDER_FIELD_OPENAI_CHAT | PROVIDER_FIELD_OPENAI_RESPONSES)

struct provider_field {
    const char *leaf;
    unsigned classes;    /* mask of enum provider_field_class */
    unsigned secret : 1; /* value must never be displayed */
};

/* The full inventory; *n receives its length. */
const struct provider_field *provider_fields(size_t *n);

/* Warn about providers.<name> members nothing consumes. A member is consumed by being in
 * `classes` — the resolved dialect's class plus the def traits the caller determined — by
 * being a registered per-provider setting, or by the NULL-terminated `extra` allowlist (may
 * be NULL). `api_label` names the dialect in the message. Warnings never fail construction,
 * so a config written for a newer hax still runs. */
void provider_warn_unused_fields(const char *name, const char *api_label, unsigned classes,
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

/* Affinity id for requests outside any conversation. Borrowed static storage, generated on
 * first use; foreground-thread only. */
const char *provider_process_session_id(void);

/* Owned NULL-terminated "Name: value" strings from an extra_headers object (a def's or a config
 * block's), or NULL when none survive. A "$NAME" value reads the environment like an inline
 * api_key; an empty value becomes a "Name:" removal marker for provider_headers_merge. Invalid
 * entries and a non-object warn, naming `label`, and drop. */
char **provider_headers_from_object(const json_t *object, const char *label);

/* <prefix>.extra_headers as templates — placeholders and markers intact — or NULL. */
char **provider_extra_header_templates(const char *config_prefix);

/* <prefix>.extra_headers ready to send on a request outside any conversation, or NULL. */
char **provider_extra_headers(const char *config_prefix);

/* Owned defaults plus overrides (either may be NULL): an override replaces a same-named default,
 * case-insensitively as HTTP requires, and a removal marker deletes one without being emitted.
 * NULL when empty. */
char **provider_headers_merge(char *const *defaults, char *const *overrides);

/* Owned copy of `templates` (may be NULL) with every "{session_id}" replaced by `session_id`
 * (non-NULL). Free with string_array_free. */
char **provider_headers_expand(char *const *templates, const char *session_id);

#endif /* HAX_PROVIDERS_PROVIDER_CONFIG_H */
