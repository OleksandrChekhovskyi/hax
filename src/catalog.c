/* SPDX-License-Identifier: MIT */
#include "catalog.h"

#include <jansson.h>
#include <libgen.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "config.h"
#include "effort.h"
#include "xalloc.h"
#include "system/bg_job.h"
#include "system/fd.h"
#include "system/fs.h"
#include "system/path.h"
#include "transport/http.h"

#define CATALOG_CACHE_FILE "catalog.json"
/* Bound a worker even when process shutdown never cancels it. */
#define CATALOG_FETCH_TIMEOUT_S 30
/* Bounds both the HTTP response and cache-file buffer. */
#define CATALOG_MAX_BYTES (32 * 1024 * 1024)
/* Keep the warning threshold well beyond the refresh interval to ignore transient failures. */
#define CATALOG_STALE_WARN_S (30L * 24 * 60 * 60)

/* ---------------- entry parsing (shared by both tiers) ---------------- */

void catalog_entry_init(struct catalog_entry *entry)
{
    memset(entry, 0, sizeof(*entry));
    entry->cost_input = -1;
    entry->cost_output = -1;
    entry->cost_cache_read = -1;
    entry->cost_cache_write = -1;
    entry->cost_cache_write_1h = -1;
    entry->image_input = CATALOG_SUPPORT_UNKNOWN;
}

/* Values arrive as JSON numbers (snapshot, typed config) or as strings a user wrote. */
static double member_rate(json_t *object, const char *name)
{
    json_t *value = json_object_get(object, name);
    if (json_is_number(value)) {
        double rate = json_number_value(value);
        return rate >= 0 ? rate : -1;
    }
    const char *text = json_string_value(value);
    if (!text || !*text)
        return -1;
    char *end;
    double rate = strtod(text, &end);
    return end != text && !*end && rate >= 0 ? rate : -1;
}

static long member_tokens(json_t *object, const char *name)
{
    json_t *value = json_object_get(object, name);
    if (json_is_integer(value)) {
        long tokens = (long)json_integer_value(value);
        return tokens > 0 ? tokens : 0;
    }
    return parse_token_count(json_string_value(value));
}

static void fill_tiers(struct catalog_entry *entry, json_t *tiers)
{
    if (entry->tiers_declared || !json_is_array(tiers))
        return;
    entry->tiers_declared = 1; /* An empty array explicitly selects flat pricing. */
    size_t index;
    json_t *tier_value;
    json_array_foreach(tiers, index, tier_value)
    {
        if (entry->n_tiers >= CATALOG_TIERS_MAX)
            break;
        if (!json_is_object(tier_value))
            continue;
        json_t *selector = json_object_get(tier_value, "tier");
        if (!json_is_object(selector))
            continue;
        /* Reject ambiguous selectors rather than applying an unexpected surcharge. */
        const char *type = json_string_value(json_object_get(selector, "type"));
        if (!type || strcmp(type, "context") != 0)
            continue;
        long threshold = member_tokens(selector, "size");
        if (threshold <= 0)
            continue;
        struct catalog_tier *tier = &entry->tiers[entry->n_tiers++];
        tier->context_threshold = threshold;
        tier->cost_input = member_rate(tier_value, "input");
        tier->cost_output = member_rate(tier_value, "output");
        tier->cost_cache_read = member_rate(tier_value, "cache_read");
        tier->cost_cache_write = member_rate(tier_value, "cache_write");
        tier->cost_cache_write_1h = member_rate(tier_value, "cache_write_1h");
    }
}

/* A budget, toggle, or `reasoning: false` is a known-empty categorical effort set. */
static void fill_efforts(struct catalog_entry *entry, json_t *model_object)
{
    if (entry->efforts.known)
        return;
    json_t *reasoning = json_object_get(model_object, "reasoning");
    if (json_is_false(reasoning)) {
        entry->efforts.known = 1;
        return;
    }
    json_t *options = json_object_get(model_object, "reasoning_options");
    if (!json_is_array(options) || json_array_size(options) == 0)
        return;
    size_t option_index;
    json_t *option;
    json_array_foreach(options, option_index, option)
    {
        const char *type = json_string_value(json_object_get(option, "type"));
        if (!type || strcmp(type, "effort") != 0)
            continue;
        json_t *values = json_object_get(option, "values");
        if (!json_is_array(values))
            continue;
        size_t value_index;
        json_t *value;
        json_array_foreach(values, value_index, value)
            effort_set_add(&entry->efforts, json_string_value(value));
    }
    entry->efforts.known = 1;
}

/* The hint is spelled `interleaved: {"field": ...}`, or as a bare field name. Only plain-string
 * members are named here: typed `reasoning_details` blocks replay from what the stream reported
 * rather than from catalog metadata, and reasoning text under that name would not parse.
 *
 * `*declared` reports a definite answer that a lower-priority source may not revise: a member this
 * function resolved, or `false` turning replay off. A hint that only asserts interleaving without
 * naming a member says nothing actionable, and so leaves the question open. */
static const char *canonical_interleaved_field(const json_t *interleaved, int *declared)
{
    static const char *const FIELDS[] = {"reasoning", "reasoning_content"};
    if (json_is_false(interleaved)) {
        *declared = 1;
        return NULL;
    }
    const char *field = json_string_value(interleaved);
    if (!field)
        field = json_string_value(json_object_get(interleaved, "field"));
    if (!field)
        return NULL;
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++)
        if (strcasecmp(field, FIELDS[i]) == 0) {
            *declared = 1;
            return FIELDS[i];
        }
    return NULL;
}

/* Normalize a declared dialect to its canonical static-storage name; an unknown name marks the
 * model unsupported rather than being guessed at, matching npm_dialect. */
static const char *canonical_api(const char *api)
{
    static const char *const DIALECTS[] = {"openai-completions", "openai-responses",
                                           "anthropic-messages"};
    for (size_t i = 0; i < sizeof(DIALECTS) / sizeof(DIALECTS[0]); i++)
        if (strcasecmp(api, DIALECTS[i]) == 0)
            return DIALECTS[i];
    return "unsupported";
}

/* Preserve known fields so higher-priority sources win field by field. */
static void fill_entry(struct catalog_entry *entry, json_t *model_object)
{
    if (!json_is_object(model_object))
        return;
    /* An explicit api member — a catalog.models override — beats the SDK-derived hint. */
    const char *api = json_string_value(json_object_get(model_object, "api"));
    if (api && !entry->api)
        entry->api = canonical_api(api);
    json_t *cost = json_object_get(model_object, "cost");
    if (json_is_object(cost)) {
        if (entry->cost_input < 0)
            entry->cost_input = member_rate(cost, "input");
        if (entry->cost_output < 0)
            entry->cost_output = member_rate(cost, "output");
        if (entry->cost_cache_read < 0)
            entry->cost_cache_read = member_rate(cost, "cache_read");
        if (entry->cost_cache_write < 0)
            entry->cost_cache_write = member_rate(cost, "cache_write");
        if (entry->cost_cache_write_1h < 0)
            entry->cost_cache_write_1h = member_rate(cost, "cache_write_1h");
        fill_tiers(entry, json_object_get(cost, "tiers"));
    }
    json_t *limit = json_object_get(model_object, "limit");
    if (json_is_object(limit)) {
        if (entry->context_window <= 0)
            entry->context_window = member_tokens(limit, "context");
        if (entry->max_output <= 0)
            entry->max_output = member_tokens(limit, "output");
    }
    fill_efforts(entry, model_object);
    if (!entry->interleaved_declared) {
        entry->interleaved_field = canonical_interleaved_field(
            json_object_get(model_object, "interleaved"), &entry->interleaved_declared);
    }
    if (entry->image_input == CATALOG_SUPPORT_UNKNOWN) {
        json_t *modalities = json_object_get(model_object, "modalities");
        json_t *inputs = json_is_object(modalities) ? json_object_get(modalities, "input") : NULL;
        if (json_is_array(inputs)) {
            entry->image_input = CATALOG_SUPPORT_NO;
            size_t index;
            json_t *modality;
            json_array_foreach(inputs, index, modality)
            {
                const char *name = json_string_value(modality);
                if (name && strcmp(name, "image") == 0)
                    entry->image_input = CATALOG_SUPPORT_YES;
            }
        }
    }
}

static int entry_has_metadata(const struct catalog_entry *entry)
{
    /* A declared tier list is a real answer even when empty: it explicitly selects flat
     * pricing and must survive to override a tiered report. */
    return entry->cost_input >= 0 || entry->cost_output >= 0 || entry->cost_cache_read >= 0 ||
           entry->cost_cache_write >= 0 || entry->cost_cache_write_1h >= 0 ||
           entry->context_window > 0 || entry->max_output > 0 ||
           entry->image_input != CATALOG_SUPPORT_UNKNOWN || entry->tiers_declared ||
           entry->efforts.known || entry->api != NULL || entry->interleaved_declared;
}

static void merge_entry(struct catalog_entry *dst, const struct catalog_entry *src)
{
    if (dst->cost_input < 0)
        dst->cost_input = src->cost_input;
    if (dst->cost_output < 0)
        dst->cost_output = src->cost_output;
    if (dst->cost_cache_read < 0)
        dst->cost_cache_read = src->cost_cache_read;
    if (dst->cost_cache_write < 0)
        dst->cost_cache_write = src->cost_cache_write;
    if (dst->cost_cache_write_1h < 0)
        dst->cost_cache_write_1h = src->cost_cache_write_1h;
    if (dst->context_window <= 0)
        dst->context_window = src->context_window;
    if (dst->max_output <= 0)
        dst->max_output = src->max_output;
    if (dst->image_input == CATALOG_SUPPORT_UNKNOWN)
        dst->image_input = src->image_input;
    if (!dst->tiers_declared && src->tiers_declared) {
        memcpy(dst->tiers, src->tiers, sizeof(dst->tiers));
        dst->n_tiers = src->n_tiers;
        dst->tiers_declared = 1;
    }
    /* Whole-list, like tiers and for the same reason: a ladder merged
     * level-by-level from two sources would offer a set that never existed
     * on any one model. */
    if (!dst->efforts.known)
        dst->efforts = src->efforts;
    if (!dst->api)
        dst->api = src->api;
    if (!dst->interleaved_declared) {
        dst->interleaved_field = src->interleaved_field;
        dst->interleaved_declared = src->interleaved_declared;
    }
}

/* ---------------- top-level member extraction ---------------- */

/* Jansson greatly inflates the full artifact, so tree-parse only the requested member. The
 * structural byte scan is UTF-8-safe because quotes and backslashes cannot occur inside multibyte
 * sequences. */

static const char *scan_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* Advance past the string whose opening '"' is at `p`. Returns the
 * position just past the closing quote, NULL on truncated input. */
static const char *scan_string(const char *p)
{
    for (p++; *p; p++) {
        if (*p == '\\') {
            if (!p[1])
                return NULL;
            p++;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

/* Advance past one JSON value starting at `p`: strings and {} / []
 * nesting are honored, everything else is structural-only. NULL on
 * truncated input. */
static const char *scan_value(const char *p)
{
    if (*p == '"')
        return scan_string(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = scan_string(p);
                if (!p)
                    return NULL;
                continue;
            }
            if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                if (--depth == 0)
                    return p + 1;
            }
            p++;
        }
        return NULL;
    }
    /* Scalar token (number / true / false / null): up to a delimiter. */
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' &&
           *p != '\r')
        p++;
    return p;
}

enum scan_member_result {
    SCAN_MEMBER_INVALID = -1,
    SCAN_MEMBER_MORE,
    SCAN_MEMBER_LAST,
};

struct scanned_member {
    const char *key;
    size_t key_length;
    const char *value_start;
    const char *value_end;
};

/* On success, `cursor` advances to the next key or past the root's closing brace. */
static enum scan_member_result scan_member(const char **cursor, struct scanned_member *member)
{
    const char *p = *cursor;
    if (*p != '"')
        return SCAN_MEMBER_INVALID;
    member->key = p + 1;
    const char *key_end = scan_string(p);
    if (!key_end)
        return SCAN_MEMBER_INVALID;
    member->key_length = (size_t)(key_end - 1 - member->key);
    p = scan_ws(key_end);
    if (*p != ':')
        return SCAN_MEMBER_INVALID;
    p = scan_ws(p + 1);
    member->value_start = p;
    p = scan_value(p);
    if (!p)
        return SCAN_MEMBER_INVALID;
    member->value_end = p;
    p = scan_ws(p);
    if (*p == ',') {
        *cursor = scan_ws(p + 1);
        return SCAN_MEMBER_MORE;
    }
    if (*p == '}') {
        *cursor = p + 1;
        return SCAN_MEMBER_LAST;
    }
    return SCAN_MEMBER_INVALID;
}

/* Position of the first member key in object `text`; NULL for an empty
 * object or a non-object. */
static const char *scan_first_member(const char *text)
{
    const char *p = scan_ws(text);
    if (*p != '{')
        return NULL;
    p = scan_ws(p + 1);
    return *p == '"' ? p : NULL;
}

json_t *catalog_extract_member(const char *text, const char *key)
{
    if (!text || !key || !*key)
        return NULL;
    size_t key_length = strlen(key);
    const char *cursor = scan_first_member(text);
    if (!cursor)
        return NULL;
    for (;;) {
        struct scanned_member member;
        enum scan_member_result result = scan_member(&cursor, &member);
        if (result == SCAN_MEMBER_INVALID)
            return NULL;
        if (member.key_length == key_length && memcmp(member.key, key, key_length) == 0)
            return json_loadb(member.value_start, (size_t)(member.value_end - member.value_start),
                              JSON_DECODE_ANY, NULL);
        if (result == SCAN_MEMBER_LAST)
            return NULL;
    }
}

/* Validate every member and trailing byte before replacing a working snapshot. Requiring a
 * provider-shaped member also rejects JSON error payloads. Parse each member separately to retain
 * the bounded-memory property of lookups. */
static int catalog_text_valid(const char *text)
{
    int has_models_object = 0;
    const char *cursor = scan_first_member(text);
    if (!cursor)
        return 0;
    for (;;) {
        struct scanned_member member;
        enum scan_member_result result = scan_member(&cursor, &member);
        if (result == SCAN_MEMBER_INVALID)
            return 0;
        json_t *value =
            json_loadb(member.value_start, (size_t)(member.value_end - member.value_start),
                       JSON_DECODE_ANY, NULL);
        if (!value)
            return 0;
        if (!has_models_object)
            has_models_object =
                json_is_object(value) && json_is_object(json_object_get(value, "models"));
        json_decref(value);
        if (result == SCAN_MEMBER_LAST)
            break;
    }
    return has_models_object && *scan_ws(cursor) == '\0';
}

/* ---------------- config tier: the catalog.models block ---------------- */

static json_t *config_provider_block(const char *provider_id)
{
    if (!provider_id || !*provider_id)
        return NULL;
    const json_t *models = config_json_node("catalog.models");
    if (!json_is_object(models))
        return NULL;
    json_t *provider = json_object_get((json_t *)models, provider_id);
    return json_is_object(provider) ? provider : NULL;
}

int catalog_config_routes_models(const char *provider_id)
{
    json_t *provider = config_provider_block(provider_id);
    if (!provider)
        return 0;
    const char *model;
    json_t *entry;
    json_object_foreach(provider, model, entry)
    {
        if (json_is_object(entry) && json_is_string(json_object_get(entry, "api")))
            return 1;
    }
    return 0;
}

static void fill_from_config(const char *provider_id, const char *model,
                             struct catalog_entry *entry)
{
    json_t *provider = config_provider_block(provider_id);
    if (provider)
        fill_entry(entry, json_object_get(provider, model));
}

/* ---------------- cache tier: the fetched snapshot ---------------- */

/* Returns a new reference, or NULL when the snapshot is unavailable or lacks the provider. */
static json_t *cache_provider_slice(const char *provider_id)
{
    char *path = xdg_hax_cache_path(CATALOG_CACHE_FILE);
    if (!path)
        return NULL;
    size_t len;
    int truncated;
    char *text = fs_read_file_capped(path, CATALOG_MAX_BYTES, &len, &truncated);
    free(path);
    if (!text)
        return NULL;
    json_t *provider = truncated ? NULL : catalog_extract_member(text, provider_id);
    free(text);
    return provider;
}

/* Map a models.dev SDK selector to the wire dialect it implies. Unknown selectors mark the
 * model unsupported rather than defaulting, so a gateway model needing an unimplemented
 * protocol fails cleanly instead of being spoken to on the wrong wire. */
static const char *npm_dialect(const char *npm)
{
    if (!npm)
        return NULL;
    if (strcmp(npm, "@ai-sdk/openai-compatible") == 0)
        return "openai-completions";
    if (strcmp(npm, "@ai-sdk/openai") == 0)
        return "openai-responses";
    if (strcmp(npm, "@ai-sdk/anthropic") == 0)
        return "anthropic-messages";
    return "unsupported";
}

static void fill_api(struct catalog_entry *entry, const json_t *provider, json_t *model_object)
{
    if (entry->api || !json_is_object(model_object))
        return;
    /* A per-model selector overrides the provider-wide one. */
    json_t *override = json_object_get(model_object, "provider");
    const char *npm = json_string_value(json_object_get(override, "npm"));
    if (!npm)
        npm = json_string_value(json_object_get((json_t *)provider, "npm"));
    entry->api = npm_dialect(npm);
}

static void fill_from_slice(const json_t *provider, const char *model, struct catalog_entry *entry)
{
    json_t *models = provider ? json_object_get(provider, "models") : NULL;
    if (json_is_object(models)) {
        json_t *model_object = json_object_get(models, model);
        fill_entry(entry, model_object);
        fill_api(entry, provider, model_object);
    }
}

static void fill_from_cache(const char *provider_id, const char *model, struct catalog_entry *entry)
{
    json_t *provider = cache_provider_slice(provider_id);
    if (!provider)
        return;
    fill_from_slice(provider, model, entry);
    json_decref(provider);
}

/* ---------------- cache-tier memo (foreground thread) ---------------- */

/* Only the foreground accesses the memo; the worker publishes refreshes via the generation. */
struct memo_entry {
    char *provider_id;
    char *model;
    struct catalog_entry entry;
    int resolved;
};

static struct memo_entry *g_memo;
static size_t g_memo_count, g_memo_capacity;

/* Bumped by the fetch worker when a fresh snapshot lands; synced on lookup
 * so memoized misses don't outlive the refresh that could turn them into
 * hits. */
static _Atomic int g_cache_generation;
static int g_memo_generation;

static void memo_clear(void)
{
    for (size_t i = 0; i < g_memo_count; i++) {
        free(g_memo[i].provider_id);
        free(g_memo[i].model);
    }
    free(g_memo);
    g_memo = NULL;
    g_memo_count = g_memo_capacity = 0;
}

static struct memo_entry *memo_find(const char *provider_id, const char *model)
{
    for (size_t i = 0; i < g_memo_count; i++)
        if (strcmp(g_memo[i].provider_id, provider_id) == 0 && strcmp(g_memo[i].model, model) == 0)
            return &g_memo[i];
    return NULL;
}

static void memo_add(const char *provider_id, const char *model, const struct catalog_entry *entry,
                     int resolved)
{
    if (g_memo_count == g_memo_capacity) {
        g_memo_capacity = g_memo_capacity ? g_memo_capacity * 2 : 4;
        g_memo = xrealloc(g_memo, g_memo_capacity * sizeof(*g_memo));
    }
    struct memo_entry *memo = &g_memo[g_memo_count++];
    memo->provider_id = xstrdup(provider_id);
    memo->model = xstrdup(model);
    memo->entry = *entry;
    memo->resolved = resolved;
}

static int cache_lookup(const char *provider_id, const char *model, struct catalog_entry *out)
{
    int generation = atomic_load(&g_cache_generation);
    if (generation != g_memo_generation) {
        memo_clear();
        g_memo_generation = generation;
    }
    struct memo_entry *memo = memo_find(provider_id, model);
    if (memo) {
        *out = memo->entry;
        return memo->resolved;
    }
    catalog_entry_init(out);
    fill_from_cache(provider_id, model, out);
    int resolved = entry_has_metadata(out);
    memo_add(provider_id, model, out, resolved);
    return resolved;
}

/* ---------------- metadata resolution ---------------- */

/* Keep single and batch resolution policy independent of how cached metadata is loaded. */
struct cache_source {
    int (*fill)(const struct cache_source *source, const char *model, struct catalog_entry *out);
    const char *provider_id;
    const json_t *slice;
};

static int cache_source_memo(const struct cache_source *source, const char *model,
                             struct catalog_entry *out)
{
    return cache_lookup(source->provider_id, model, out);
}

static int cache_source_slice(const struct cache_source *source, const char *model,
                              struct catalog_entry *out)
{
    catalog_entry_init(out);
    fill_from_slice(source->slice, model, out);
    return entry_has_metadata(out);
}

/* Config fields take precedence; a NULL cache source resolves from config alone. */
static int resolve_entry(const char *provider_id, const char *catalog_id, const char *model,
                         const struct cache_source *cache, struct catalog_entry *out)
{
    catalog_entry_init(out);
    if (!model || !*model)
        return 0;
    fill_from_config(provider_id, model, out);
    if (!catalog_id || !provider_id || strcmp(provider_id, catalog_id) != 0)
        fill_from_config(catalog_id, model, out);
    /* Always consult the cache: some fields (the SDK-derived api hint) exist only there, and
     * merging fills gaps without disturbing configured values. */
    struct catalog_entry cached;
    if (cache && cache->fill(cache, model, &cached))
        merge_entry(out, &cached);
    return entry_has_metadata(out);
}

int catalog_lookup(const char *provider_id, const char *catalog_id, const char *model,
                   struct catalog_entry *out)
{
    struct cache_source memo = {.fill = cache_source_memo, .provider_id = catalog_id};
    int has_snapshot = catalog_id && *catalog_id;
    return resolve_entry(provider_id, catalog_id, model, has_snapshot ? &memo : NULL, out) ? 0 : -1;
}

int catalog_lookup_config(const char *provider_id, const char *catalog_id, const char *model,
                          struct catalog_entry *out)
{
    return resolve_entry(provider_id, catalog_id, model, NULL, out) ? 0 : -1;
}

void catalog_lookup_many(const char *provider_id, const char *catalog_id, const char *const *models,
                         size_t model_count, struct catalog_entry *out, int *found)
{
    for (size_t i = 0; i < model_count; i++) {
        catalog_entry_init(&out[i]);
        if (found)
            found[i] = 0;
    }
    if (model_count == 0)
        return;

    /* A missing provider slice falls back to config without retrying the file per model. */
    json_t *provider = catalog_id && *catalog_id ? cache_provider_slice(catalog_id) : NULL;
    struct cache_source cache = {
        .fill = cache_source_slice, .provider_id = catalog_id, .slice = provider};
    for (size_t i = 0; i < model_count; i++) {
        int resolved =
            resolve_entry(provider_id, catalog_id, models[i], provider ? &cache : NULL, &out[i]);
        if (found)
            found[i] = resolved;
    }
    json_decref(provider);
}

struct price_rates {
    double input;
    double output;
    double cache_read;
    double cache_write;
    double cache_write_1h;
};

static struct price_rates price_rates_for_input(const struct catalog_entry *entry,
                                                long input_tokens)
{
    struct price_rates rates = {
        .input = entry->cost_input,
        .output = entry->cost_output,
        .cache_read = entry->cost_cache_read,
        .cache_write = entry->cost_cache_write,
        .cache_write_1h = entry->cost_cache_write_1h,
    };
    long matched_threshold = -1;
    for (int i = 0; i < entry->n_tiers; i++) {
        const struct catalog_tier *tier = &entry->tiers[i];
        if (tier->context_threshold <= 0 || input_tokens <= tier->context_threshold ||
            tier->context_threshold <= matched_threshold)
            continue;
        matched_threshold = tier->context_threshold;
        rates.input = tier->cost_input >= 0 ? tier->cost_input : entry->cost_input;
        rates.output = tier->cost_output >= 0 ? tier->cost_output : entry->cost_output;
        rates.cache_read =
            tier->cost_cache_read >= 0 ? tier->cost_cache_read : entry->cost_cache_read;
        rates.cache_write =
            tier->cost_cache_write >= 0 ? tier->cost_cache_write : entry->cost_cache_write;
        rates.cache_write_1h =
            tier->cost_cache_write_1h >= 0 ? tier->cost_cache_write_1h : entry->cost_cache_write_1h;
    }
    return rates;
}

static int cache_write_replaces_input(double input_rate, double write_rate)
{
    return input_rate < 0 || write_rate < 0 || write_rate >= input_rate;
}

int catalog_cache_write_replaces_input(const struct catalog_entry *entry)
{
    return cache_write_replaces_input(entry->cost_input, entry->cost_cache_write);
}

double catalog_price(const struct catalog_entry *entry, long input_tokens, long output_tokens,
                     long cache_read_tokens, long cache_write_tokens, long cache_write_1h_tokens,
                     struct catalog_split *split)
{
    if (split)
        *split = (struct catalog_split){0};

    struct price_rates rates = price_rates_for_input(entry, input_tokens);
    if (rates.input < 0 || rates.output < 0)
        return -1;

    input_tokens = input_tokens > 0 ? input_tokens : 0;
    output_tokens = output_tokens > 0 ? output_tokens : 0;
    cache_read_tokens = cache_read_tokens > 0 ? cache_read_tokens : 0;
    cache_write_tokens = cache_write_tokens > 0 ? cache_write_tokens : 0;
    cache_write_1h_tokens = cache_write_1h_tokens > 0 ? cache_write_1h_tokens : 0;
    if (cache_write_1h_tokens > cache_write_tokens)
        cache_write_1h_tokens = cache_write_tokens;

    if (rates.cache_read < 0)
        rates.cache_read = rates.input;
    int writes_replace_input = cache_write_replaces_input(rates.input, rates.cache_write);
    if (rates.cache_write < 0)
        rates.cache_write = rates.input;
    if (rates.cache_write_1h < 0)
        rates.cache_write_1h = 2 * rates.input; /* Anthropic's standard 1h write multiplier. */

    long uncached_input_tokens =
        input_tokens - cache_read_tokens - (writes_replace_input ? cache_write_tokens : 0);
    if (uncached_input_tokens < 0)
        uncached_input_tokens = 0;

    double input_cost = (double)uncached_input_tokens * rates.input / 1e6;
    double cache_read_cost = (double)cache_read_tokens * rates.cache_read / 1e6;
    double cache_write_cost =
        ((double)(cache_write_tokens - cache_write_1h_tokens) * rates.cache_write +
         (double)cache_write_1h_tokens * rates.cache_write_1h) /
        1e6;
    double output_cost = (double)output_tokens * rates.output / 1e6;
    if (split) {
        split->uncached_input_tokens = uncached_input_tokens;
        split->cost_input = input_cost;
        split->cost_cache_read = cache_read_cost;
        split->cost_cache_write = cache_write_cost;
        split->cost_output = output_cost;
    }
    return input_cost + cache_read_cost + cache_write_cost + output_cost;
}

/* ---------------- background fetch ---------------- */

static struct bg_job *g_fetch_job;
static int g_prefetch_attempted;
/* bg_job has no timed join, so catalog_drain polls this worker-owned flag. */
static _Atomic int g_fetch_done;

struct fetch_args {
    char *url;
    char *path;
};

static void fetch_args_free(struct fetch_args *args)
{
    if (!args)
        return;
    free(args->url);
    free(args->path);
    free(args);
}

/* Rename a sibling temporary file so concurrent readers never observe a partial snapshot. */
static int write_cache_atomic(const char *path, const char *body, size_t body_length)
{
    char *path_copy = xstrdup(path);
    fs_mkdir_p(dirname(path_copy));
    free(path_copy);

    char *temp_path = xasprintf("%s.tmp.XXXXXX", path);
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        free(temp_path);
        return -1;
    }
    int result = fd_write_all(fd, body, body_length);
    if (close(fd) != 0)
        result = -1;
    if (result == 0 && rename(temp_path, path) != 0)
        result = -1;
    if (result != 0)
        unlink(temp_path);
    free(temp_path);
    return result;
}

static void fetch_worker(struct bg_job *job, void *arg)
{
    struct fetch_args *args = arg;
    if (!bg_job_cancel_requested(job)) {
        char *body = NULL;
        if (http_get(args->url, NULL, CATALOG_FETCH_TIMEOUT_S, CATALOG_MAX_BYTES,
                     bg_job_cancel_tick, job, &body, NULL) == 0 &&
            body) {
            if (catalog_text_valid(body) && write_cache_atomic(args->path, body, strlen(body)) == 0)
                atomic_fetch_add(&g_cache_generation, 1);
        }
        free(body);
    }
    fetch_args_free(args);
    atomic_store(&g_fetch_done, 1);
}

long catalog_prefetch(void)
{
    if (g_prefetch_attempted)
        return 0;
    g_prefetch_attempted = 1;

    const char *url = config_str("catalog.url");
    if (!url || !*url)
        return 0;
    long refresh_ms = config_duration_ms("catalog.refresh");
    if (refresh_ms <= 0)
        return 0;
    char *path = xdg_hax_cache_path(CATALOG_CACHE_FILE);
    if (!path)
        return 0;

    long stale_days = 0;
    struct stat status;
    if (stat(path, &status) == 0) {
        long snapshot_age_s = (long)(time(NULL) - status.st_mtime);
        if (snapshot_age_s < refresh_ms / 1000) {
            free(path);
            return 0;
        }
        if (snapshot_age_s > CATALOG_STALE_WARN_S)
            stale_days = snapshot_age_s / (24L * 60 * 60);
    }

    struct fetch_args *args = xcalloc(1, sizeof(*args));
    args->url = xstrdup(url);
    args->path = path;
    g_fetch_job = bg_job_spawn(fetch_worker, args);
    if (!g_fetch_job)
        fetch_args_free(args);
    return stale_days;
}

void catalog_wait(long max_wait_ms)
{
    if (!g_fetch_job)
        return;
    for (long waited_ms = 0; waited_ms < max_wait_ms && !atomic_load(&g_fetch_done);
         waited_ms += 20) {
        struct timespec delay = {0, 20 * 1000 * 1000};
        nanosleep(&delay, NULL);
    }
}

void catalog_drain(long max_wait_ms)
{
    if (!g_fetch_job)
        return;
    catalog_wait(max_wait_ms);
    if (!atomic_load(&g_fetch_done))
        bg_job_cancel(g_fetch_job);
    bg_job_join(g_fetch_job);
    g_fetch_job = NULL;
}

void catalog_shutdown(void)
{
    if (g_fetch_job) {
        bg_job_cancel(g_fetch_job);
        bg_job_join(g_fetch_job);
        g_fetch_job = NULL;
    }
    memo_clear();
    g_memo_generation = atomic_load(&g_cache_generation);
}
