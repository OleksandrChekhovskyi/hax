/* SPDX-License-Identifier: MIT */
#ifndef HAX_CATALOG_H
#define HAX_CATALOG_H

#include <jansson.h>

/*
 * Model-metadata catalog: per-model cost rates, window limits, and input
 * modalities, resolved from two tiers — user config over a cached
 * models.dev snapshot:
 *
 *   - config: the `catalog.models` block in config.json (nested under the
 *     same `catalog` namespace as catalog.url/catalog.refresh), keyed by
 *     catalog provider id then model id, mirroring the models.dev
 *     per-model field names so values can be pasted verbatim:
 *
 *       "catalog": {
 *         "models": {
 *           "openai": {
 *             "gpt-5.2-codex": {
 *               "cost": {"input": 1.25, "output": 10, "cache_read": 0.125},
 *               "limit": {"context": 400000, "output": 128000}
 *             }
 *           }
 *         }
 *       }
 *
 *   - catalog cache: $XDG_CACHE_HOME/hax/catalog.json, a verbatim copy of
 *     the catalog.url artifact (models.dev api.json by default), fetched
 *     in the background and refreshed when older than catalog.refresh.
 *
 * Config fields win; the catalog fills whatever the user didn't declare.
 * Both tiers fail soft: an absent block, missing cache file, or unknown
 * model simply leaves fields unknown, and consumers (cost estimation, the
 * context-% display) skip what they can't resolve.
 *
 * The provider id is a *catalog* identity (models.dev's provider key),
 * distinct from hax's provider name: codex and openai both map to
 * "openai". Providers declare it via provider->catalog_id; NULL opts out
 * (local backends, providers that opted out explicitly).
 *
 * The multi-MB artifact is never tree-parsed whole (jansson inflates JSON
 * ~10x in memory): lookups scan the raw bytes for the one top-level
 * provider member they need and parse only that slice — a few ms, cheap
 * enough for the foreground path, memoized per (provider, model). The
 * catalog can therefore grow without a parse-cost ceiling; the only bound
 * left is the download/buffer cap. Threading reduces to one background
 * fetch worker that touches nothing but the cache file and an atomic
 * generation counter invalidating the foreground memo.
 */

/* One long-context pricing tier: replacement rates that apply to the
 * whole request once its total input exceeds `above` tokens. Mirrors the
 * models.dev `cost.tiers` shape ({rates..., tier: {type: "context",
 * size: N}}). Negative rates fall back to the base entry's. */
struct catalog_tier {
    long above;
    double cost_input;
    double cost_output;
    double cost_cache_read;
    double cost_cache_write;
};

#define CATALOG_TIERS_MAX 4

struct catalog_entry {
    /* USD per 1M tokens; negative = unknown. */
    double cost_input;
    double cost_output;
    double cost_cache_read;  /* rate for prefix-cache read tokens */
    double cost_cache_write; /* rate for cache write tokens */
    /* Window limits in tokens; 0 = unknown. */
    long context;
    long output;
    /* Does the model accept image input (models.dev `modalities.input`
     * contains "image")? 1 = yes, 0 = no, -1 = unknown (the model object
     * doesn't declare modalities). */
    int image_input;
    /* Context tiers, in artifact order; none for flat-priced models. The
     * list is taken whole from whichever tier *declares* one first
     * (config over cache) — tiers don't merge field-by-field the way
     * base rates do, since a mixed list would price against rates that
     * never coexisted. Declaring is distinct from having entries:
     * "tiers": [] in config declares an empty list, pinning flat pricing
     * over whatever tiers the cached snapshot carries. */
    struct catalog_tier tiers[CATALOG_TIERS_MAX];
    int n_tiers;
    int tiers_declared;
};

/* Resolve metadata for (provider_id, model). Fills *out (unknown fields
 * get the sentinels above) and returns 0 when at least one field resolved,
 * -1 when the model is unknown to both tiers. Cache-tier results —
 * including misses — are memoized until a background refresh lands, so
 * per-render calls are cheap: the cache file is scanned at most once per
 * (provider, model) per generation. */
int catalog_lookup(const char *provider_id, const char *model, struct catalog_entry *out);

/* Scan `text` (a JSON object) for the top-level member named `key` and
 * tree-parse only that member's value — the primitive that keeps the
 * multi-MB artifact from ever being parsed whole. Byte-level scan: strings
 * (with escapes) and brace/bracket nesting are honored; grammar validation
 * of the slice is jansson's. Returns a new reference (caller json_decref)
 * or NULL when absent/malformed. Exposed for unit tests. */
json_t *catalog_extract_member(const char *text, const char *key);

/* Per-category component split of one priced request, USD. `in` is the
 * uncached input remainder; `cache_write` includes the 2x-rate 1h subset. */
struct catalog_split {
    double in;
    double cache_read;
    double cache_write;
    double out;
};

/* Price ONE request against an entry's rates, in USD; -1 when the
 * input or output rate is unknown (no estimate is better than a wildly
 * partial one). `cached` (prefix-cache reads) and `cache_write` are the
 * non-overlapping subsets of `input` that stream_usage reports; they are
 * priced at their own rates, falling back to the input rate when the
 * entry doesn't declare one. `cache_write_1h` is the 1h-TTL subset of
 * `cache_write`, priced at 2x the input rate — Anthropic's documented
 * multiplier; the catalog's cache_write rate covers only the default
 * 5-minute writes and models.dev carries no 1h rate to read instead.
 * Negative token counts (the "not reported" convention) read as 0.
 *
 * Tier selection is why this prices one request, never an aggregate:
 * `input` (cache subsets included) picks the highest tier it exceeds and
 * the whole request bills at that tier's rates — summed batches would
 * cross thresholds their individual requests never did. When non-NULL,
 * *split receives the per-category component costs (zeros when the total
 * is -1). Pure math — no I/O. */
double catalog_price(const struct catalog_entry *e, long input, long output, long cached,
                     long cache_write, long cache_write_1h, struct catalog_split *split);

/* Spawn the once-per-run background snapshot fetch if the cache file is
 * missing or older than catalog.refresh (empty catalog.url or zero
 * refresh opt out). Call when a stream is about to need metadata — the
 * agent does, once per run, for providers with a catalog_id. Fetch
 * failure is silent (a stale cache keeps serving; no cache means lookups
 * miss), but a *persistently* failing refresh is surfaced: returns the
 * snapshot's age in days when it exceeds the staleness alarm (~30 days —
 * whatever the cause: endpoint gone, artifact over the size cap, broken
 * proxy), else 0. The caller owns presenting that warning through its own
 * channel (ui_note in the REPL, stderr in -p). */
long catalog_prefetch(void);

/* Give an in-flight background fetch up to `max_wait_ms` to finish
 * WITHOUT cancelling it (polled, ~20 ms granularity), then settle its
 * handle — a fetch still running at the bound is cancelled exactly as
 * catalog_shutdown would. No-op when no fetch is in flight. For the
 * one-shot exit path: a short cold-cache -p run would otherwise compute
 * its estimate against an empty cache and then cancel the download at
 * shutdown — on every run, so the cache could never populate. The
 * interactive REPL never calls this (exit stays prompt; its sessions are
 * long enough for the fetch to land on its own). */
void catalog_drain(long max_wait_ms);

/* Cancel/join the background fetch and free the memo. Call once at
 * process teardown, before curl_global_cleanup (the fetch worker holds a
 * libcurl handle) — this is for ASan-clean shutdown and prompt cancel of
 * an in-flight transfer. */
void catalog_shutdown(void);

#endif /* HAX_CATALOG_H */
