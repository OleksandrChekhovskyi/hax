/* SPDX-License-Identifier: MIT */
#ifndef HAX_CATALOG_H
#define HAX_CATALOG_H

#include <jansson.h>

#include "effort.h"

/* The model catalog resolves per-model pricing, token limits, image support, and reasoning-effort
 * metadata. User `catalog.models` configuration takes precedence over a cached models.dev
 * snapshot; missing providers, models, and fields remain unknown. Lookups take two keys: the
 * runtime provider id scopes configuration to one provider, and the models.dev `catalog_id` names
 * the snapshot identity, which several providers may share. The prefetch lifecycle refreshes the
 * cached snapshot asynchronously. */

#define CATALOG_TIERS_MAX 4

/* Rate overrides for requests whose total input exceeds a positive `context_threshold`. */
struct catalog_tier {
    long context_threshold;
    double cost_input;
    double cost_output;
    double cost_cache_read;
    double cost_cache_write;
    double cost_cache_write_1h;
};

enum catalog_support {
    CATALOG_SUPPORT_UNKNOWN = -1,
    CATALOG_SUPPORT_NO,
    CATALOG_SUPPORT_YES,
};

/* Metadata merged from catalog.models configuration over the cached models.dev snapshot. */
struct catalog_entry {
    /* USD per million tokens; negative = unknown. */
    double cost_input;
    double cost_output;
    double cost_cache_read;
    double cost_cache_write;
    double cost_cache_write_1h;

    /* Token limits; 0 = unknown. */
    long context_window;
    long max_output;

    enum catalog_support image_input;

    /* Canonical dialect name of the wire the model's endpoint speaks (e.g.
     * "anthropic-messages"), "unsupported" for SDK selectors hax does not implement, or NULL
     * when the snapshot reports none. Static storage; mixed-protocol gateways consult it. */
    const char *api;

    /* `known` distinguishes an unsupported effort ladder from absent metadata. */
    struct effort_set efforts;

    /* Chat Completions member that must carry an assistant turn's own reasoning back to the
     * model: interleaved-thinking models stop reasoning once it is missing from their history.
     * Static storage, "reasoning" or "reasoning_content"; NULL when the model needs no replay
     * or round-trips typed blocks instead. */
    const char *interleaved_field;
    int interleaved_declared; /* Distinguishes an absent hint from one declared off. */

    /* A declared list replaces the lower-priority list rather than merging with it. */
    struct catalog_tier tiers[CATALOG_TIERS_MAX];
    int n_tiers;
    int tiers_declared; /* Distinguishes an absent list from a declared empty list. */
};

/* Initialize every field to its documented unknown state. */
void catalog_entry_init(struct catalog_entry *entry);

/* Resolve one model. Configuration under `provider_id` wins field by field over configuration
 * under `catalog_id`, which wins over the cached snapshot (keyed by `catalog_id` alone). Either
 * key may be NULL or empty. Returns 0 when any metadata resolved and -1 otherwise. `out` is always
 * initialized. Results from the snapshot, including misses, are memoized until a successful
 * refresh. */
int catalog_lookup(const char *provider_id, const char *catalog_id, const char *model,
                   struct catalog_entry *out);

/* Resolve one model from the catalog.models configuration tier alone, ignoring the snapshot.
 * Callers use this entry to rank explicit configuration above provider-reported metadata. Same
 * keying and return convention as catalog_lookup. */
int catalog_lookup_config(const char *provider_id, const char *catalog_id, const char *model,
                          struct catalog_entry *out);

/* Return whether catalog.models configuration names a wire dialect for any model under
 * `provider_id`. Cost or limit overrides alone cannot route a catalog-wired provider. */
int catalog_config_routes_models(const char *provider_id);

/* Resolve `model_count` models for one provider while loading its cached snapshot once. `models`
 * and `out` must contain `model_count` elements. If non-NULL, `found[i]` receives 1 when any
 * metadata resolved and 0 otherwise. NULL or empty model IDs are unresolved. */
void catalog_lookup_many(const char *provider_id, const char *catalog_id, const char *const *models,
                         size_t model_count, struct catalog_entry *out, int *found);

/* Parse the top-level member named `key` without tree-parsing the full JSON object. Returns a new
 * reference, or NULL when the member is absent or malformed. The caller must call json_decref. */
json_t *catalog_extract_member(const char *text, const char *key);

/* Return whether cache writes replace the input charge. A known write rate below the input rate is
 * treated as a storage surcharge; unknown rates use the more common replacement policy. */
int catalog_cache_write_replaces_input(const struct catalog_entry *entry);

/* Per-category costs for one request, in USD. */
struct catalog_split {
    double cost_input;
    double cost_cache_read;
    double cost_cache_write;
    double cost_output;
    long uncached_input_tokens;
};

/* Price one request in USD. Cache-read and cache-write counts are subsets of input, and
 * `cache_write_1h_tokens` is a subset of cache writes. Negative counts are treated as zero. The
 * highest context tier exceeded by total input replaces the base rates for the whole request, with
 * unknown tier rates falling back to their base rates. An unknown 1h write rate uses twice the
 * input rate. Returns -1 unless input and output rates are known. If non-NULL, `split` is zeroed
 * even when pricing fails. */
double catalog_price(const struct catalog_entry *entry, long input_tokens, long output_tokens,
                     long cache_read_tokens, long cache_write_tokens, long cache_write_1h_tokens,
                     struct catalog_split *split);

/* Start the process-wide background refresh when the snapshot is older than catalog.refresh.
 * Empty catalog.url or a non-positive refresh interval disables fetching. Only the first call per
 * process does work. Returns the stale snapshot's age in days once it exceeds the warning
 * threshold, otherwise 0. Fetch failures leave the existing snapshot untouched. */
long catalog_prefetch(void);

/* Wait up to `max_wait_ms` for a background refresh to land, leaving a slower fetch running for
 * later callers. No-op when no refresh is running. */
void catalog_wait(long max_wait_ms);

/* Give short-lived runs up to `max_wait_ms` to finish a background refresh, then cancel and join
 * it. No-op when no refresh is running. */
void catalog_drain(long max_wait_ms);

/* Cancel and join the background refresh, then clear memoized lookups. Must run before
 * curl_global_cleanup(). */
void catalog_shutdown(void);

#endif /* HAX_CATALOG_H */
