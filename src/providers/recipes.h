/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_RECIPES_H
#define HAX_PROVIDERS_RECIPES_H

#include <stddef.h>

struct provider;

/* A built-in recipe: the default field values for a well-known provider, overridable
 * key-by-key by a matching providers.<name> config block. The shipped set is deliberately
 * small and slow-moving — only providers fully described by static endpoint metadata (no
 * auth/transport code), since hax discovers models live via /v1/models rather than shipping
 * a catalog. Borrowed static strings throughout; the recipe table outlives every provider
 * built from it. */
struct provider_recipe {
    const char *id;               /* selectable HAX_PROVIDER value */
    const char *display_name;     /* banner label; NULL → name */
    const char *api;              /* dialect: openai-completions | openai-responses |
                                     anthropic-messages, or catalog for per-model routing */
    const char *base_url;         /* default endpoint; NULL → the user must configure one */
    const char *api_key_env;      /* env var holding the key; NULL → local/no key */
    const char *reasoning_format; /* "flat"/"nested"; NULL → flat */
    const char *catalog_id;       /* models.dev key (catalog.h); in a recipe, NULL is a
                                     curated absence — no fallback to `name` */
    int send_cache_key;           /* prompt_cache_key default (0/1) */
    const char *length_hint;      /* appended to a "length"-truncation error */
    int no_efforts;               /* offer no effort levels, so /effort skips the provider */
    /* Probe <base_url>/models reachability when keyless. Only for curated local recipes where
     * "not running" is the common failure and /models is known to exist; a generic endpoint may
     * not serve /models at all, so configuration is the default availability check. */
    int probe;
    const char *unconfigured_reason; /* availability reason without a base_url; NULL →
                                        "no base_url" */
    /* /usage backend copied onto the constructed provider; NULL leaves /usage unsupported. */
    int (*query_usage)(struct provider *provider);
};

/* The shipped table, in /provider display order; *n receives its length. */
const struct provider_recipe *provider_recipes(size_t *n);

/* The recipe named `id`, or an all-defaults stand-in so field lookups need no NULL checks;
 * the stand-in's NULL `id` still marks the recipe's absence where that matters. */
const struct provider_recipe *provider_recipe_find(const char *id);

#endif /* HAX_PROVIDERS_RECIPES_H */
