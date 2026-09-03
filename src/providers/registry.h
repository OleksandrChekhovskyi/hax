/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_REGISTRY_H
#define HAX_PROVIDERS_REGISTRY_H

#include <jansson.h>
#include <stdio.h>

#include "provider.h"

struct http_auth_source; /* providers/http_provider.h */

/* Provider definitions.
 *
 * Every provider is described by one struct provider_def: default field values overridable
 * key-by-key by a matching providers.<id> config block, plus optional hooks for genuinely
 * provider-specific behavior. The shipped defs live in registry.c's table; each config.json
 * providers.<name> block either overlays the shipped def of the same name or adds a new
 * data-only def. Most defs are pure data; hooks exist only in the shipped table.
 *
 * Borrowed static strings throughout; a def outlives every provider built from it. */
struct provider_def {
    const char *id;           /* selectable HAX_PROVIDER value; provider->id carries it */
    const char *display_name; /* banner/picker label; NULL → id */
    /* Hidden from enumeration and automatic selection, but still resolvable by name. */
    int internal;

    /* Default field values, named after their providers.<id> config leaves. */
    const char *api; /* dialect: openai-completions (the default) | openai-responses |
                        anthropic-messages, or catalog for per-model routing */
    /* NULL → the user must configure one. A "{port}" placeholder expands to the resolved
     * providers.<id>.port, for local servers addressed by port. A configured base_url is
     * always used verbatim. */
    const char *base_url;
    int port; /* default for the "{port}" placeholder (0: none); providers.<id>.port overrides */
    /* First-party endpoint: configured base_url and api cannot rewire it, so its key is never
     * redirected to another host or protocol family. */
    int pinned;
    const char *api_key_env;  /* env var holding the key; NULL → local/no key */
    const char *catalog_id;   /* models.dev key (catalog.h). In a shipped def, NULL is a curated
                                 absence; a config-only def defaults to its own name. */
    const char *metadata_api; /* /models dialect: "openai" (flat list) or "anthropic" (paged);
                                 NULL follows the default wire's family */
    int send_cache_key;       /* prompt_cache_key default (0/1) */
    /* Cache-marker default: "auto" plans chat markers from model rates, "on" always sends them;
     * NULL sends none. */
    const char *cache;
    int request_cost;             /* chat: request provider-reported per-response cost */
    const char *reasoning_format; /* "flat"/"nested"; NULL → flat */
    /* Chat default for the member prior reasoning replays under; NULL disables replay.
     * providers.<id>.reasoning_roundtrip overrides either way. */
    const char *reasoning_roundtrip;
    const char *thinking_mode; /* messages: "adaptive"/"budget"/"off"; NULL → compat-safe budget */
    /* The endpoint signs and validates thinking blocks like the first-party Messages API, so
     * unsigned blocks from other backends are dropped rather than replayed and rejected. */
    int strict_signatures;
    const char *length_hint; /* appended to a "length"-truncation error */
    int no_efforts;          /* offer no effort levels, so /effort skips the provider */
    /* JSON object of body members the endpoint requires on every request, merged under the
     * user's providers.<id>.extra_body. NULL sends none. */
    const char *extra_body;
    /* JSON object of headers the endpoint expects on every request, overridden by name by the
     * user's providers.<id>.extra_headers; "{session_id}" in a value expands to the conversation's
     * affinity id. NULL sends none. */
    const char *extra_headers;
    /* Probe <base_url>/models reachability when keyless. Only for curated local defs where
     * "not running" is the common failure and /models is known to exist; a generic endpoint may
     * not serve /models at all, so configuration is the default availability check. */
    int probe;
    const char *unconfigured_reason; /* availability reason without a base_url; NULL →
                                        "no base_url" */

    /* Capability hooks the generic constructor installs on the built provider; NULL keeps the
     * generic behavior. A def with a construct override wires its provider itself instead.
     * parse_model, probe_model, and list_models refine the def's own metadata dialect and stand
     * down when a configured metadata_api moves the provider to the other one. */
    void (*parse_model)(const json_t *entry, struct model_info *out); /* refine one /models entry */
    int (*probe_model)(struct provider *provider, const char *model, struct model_probe *probe);
    int (*list_models)(struct provider *provider, struct model_info **models, size_t *n_models,
                       char **error, http_tick_cb tick, void *tick_user);
    int (*query_usage)(struct provider *provider);
    char *(*model_label)(struct provider *provider, const char *model);
    /* Create the provider's dynamic credential source (http_provider.h), or return non-zero
     * after reporting why no usable credentials exist, failing construction. A def with an
     * auth source resolves no API key. */
    int (*auth_source)(const struct provider_def *def, struct http_auth_source *out);
    /* Resolve model/effort defaults mirrored from live companion-tool state into owned
     * outputs; NULL leaves the provider without defaults. */
    void (*load_defaults)(char **default_model, char **default_effort);
    /* Reconcile live server state after endpoint resolution, before the provider is built: may
     * adopt or correct the active model from the running server, with `model_discovered`
     * marking a model read from transient server state that must not be persisted. Return
     * non-zero after reporting why construction must fail. */
    int (*discover)(const char *base_url, int *model_discovered);

    /* Whole-provider overrides for providers the data path cannot express. */
    struct provider *(*construct)(const struct provider_def *def);
    /* Prepare an immediate verdict or an owned GET request on the foreground thread. NULL uses
     * the generic data-driven check — or, with a construct override, immediate availability. */
    void (*prepare_availability)(const struct provider_def *def, struct provider_availability *out);
};

/* Look up a shipped or config-defined def by name, accepting former ids. Shipped names take
 * precedence; returns NULL when no provider is registered. */
const struct provider_def *provider_find(const char *name);

/* Display label without constructing the provider: the configured providers.<id>.display_name,
 * else the def's default, else the id. Borrowed; valid until the next config write. */
const char *provider_display_name(const struct provider_def *def);

/* Return the highest-priority user-facing def, or NULL when none is registered. */
const struct provider_def *provider_default(void);

/* Return the read-only user-facing registry in autoselect order; internal defs are excluded.
 * `count` receives its length. Foreground-thread only. */
const struct provider_def *const *provider_all(size_t *count);

/* Write the space-separated user-facing provider names in autoselect order. */
void provider_list_names(FILE *out);

/* Build `def`'s provider — its construct override or the generic data-driven constructor —
 * and apply the shared base config (providers.<id>.sort_models). NULL on failure. */
struct provider *provider_construct(const struct provider_def *def);

/* Availability for pickers and autoselect: the def's own hook, else immediate availability for
 * a construct override, else the generic data-driven check. `out` need not be initialized. */
void provider_prepare_availability(const struct provider_def *def,
                                   struct provider_availability *out);

#endif /* HAX_PROVIDERS_REGISTRY_H */
