/* SPDX-License-Identifier: MIT */
#include "agent_usage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "catalog.h"
#include "model_meta.h"

struct spend_record {
    struct stream_usage usage;
    double reported_cost; /* -1 when the provider did not report cost */
    struct catalog_entry rates;
    int has_rates;
    char *catalog_id;
    char *model;
};

int agent_format_stats_segments(char segments[][AGENT_STATS_SEGMENT_LEN], long context_tokens,
                                long context_limit, long elapsed_ms, double session_spend,
                                int spend_estimated)
{
    int count = 0;
    char value[AGENT_STATS_SEGMENT_LEN - 16];

    if (elapsed_ms >= 0) {
        format_duration(value, sizeof(value), elapsed_ms);
        snprintf(segments[count++], AGENT_STATS_SEGMENT_LEN, "%s", value);
    }
    if (context_tokens >= 0) {
        format_context(value, sizeof(value), context_tokens, context_limit);
        if (context_limit <= 0)
            snprintf(segments[count++], AGENT_STATS_SEGMENT_LEN, "context %s", value);
        else
            snprintf(segments[count++], AGENT_STATS_SEGMENT_LEN, "%s", value);
    }
    if (session_spend > 0) {
        format_cost(value, sizeof(value), session_spend);
        snprintf(segments[count++], AGENT_STATS_SEGMENT_LEN, "%s%s", spend_estimated ? "~" : "",
                 value);
    }
    return count;
}

void agent_spend_account(struct spend_totals *totals, const struct stream_usage *usage,
                         const struct provider *provider, const char *model)
{
    if (usage->cost < 0 && usage->input_tokens <= 0 && usage->output_tokens <= 0)
        return;

    if (totals->count == totals->capacity) {
        totals->capacity = totals->capacity ? totals->capacity * 2 : 8;
        totals->records = xrealloc(totals->records, totals->capacity * sizeof(*totals->records));
    }

    struct spend_record *record = &totals->records[totals->count++];
    record->usage = *usage;
    record->reported_cost = usage->cost;
    record->has_rates = model_meta_rates(provider, model, &record->rates);
    const char *catalog_id = provider ? provider->catalog_id : NULL;
    record->catalog_id = catalog_id && *catalog_id ? xstrdup(catalog_id) : NULL;
    record->model = model && *model ? xstrdup(model) : NULL;
}

/* Return an estimated token cost, or -1 when the record cannot be priced. */
static double spend_record_estimate(const struct spend_record *record, struct catalog_split *split)
{
    const struct stream_usage *usage = &record->usage;
    if (record->has_rates)
        return catalog_price(&record->rates, usage->input_tokens, usage->output_tokens,
                             usage->cached_tokens, usage->cache_write_tokens,
                             usage->cache_write_1h_tokens, split);
    if (!record->catalog_id || !record->model)
        return -1;

    struct catalog_entry rates;
    if (catalog_lookup(record->catalog_id, record->model, &rates) != 0)
        return -1;
    return catalog_price(&rates, usage->input_tokens, usage->output_tokens, usage->cached_tokens,
                         usage->cache_write_tokens, usage->cache_write_1h_tokens, split);
}

static double spend_record_total(const struct spend_record *record, int *exact)
{
    if (record->reported_cost >= 0) {
        if (exact)
            *exact = 1;
        return record->reported_cost;
    }
    if (exact)
        *exact = 0;
    return spend_record_estimate(record, NULL);
}

double agent_spend_total(const struct spend_totals *totals, int *estimated)
{
    double total = 0;
    int has_estimate = 0;

    for (size_t i = 0; i < totals->count; i++) {
        int exact = 0;
        double cost = spend_record_total(&totals->records[i], &exact);
        if (cost >= 0)
            total += cost;
        if (!exact)
            has_estimate = 1;
    }
    if (estimated)
        *estimated = has_estimate;
    return total;
}

int agent_spend_has_unpriced(const struct spend_totals *totals)
{
    for (size_t i = 0; i < totals->count; i++)
        if (spend_record_total(&totals->records[i], NULL) < 0)
            return 1;
    return 0;
}

int agent_spend_split(const struct spend_totals *totals, struct catalog_split *split)
{
    *split = (struct catalog_split){0};
    int has_priced_record = 0;

    for (size_t i = 0; i < totals->count; i++) {
        struct catalog_split record_split;
        if (spend_record_estimate(&totals->records[i], &record_split) < 0)
            continue;
        split->in += record_split.in;
        split->cache_read += record_split.cache_read;
        split->cache_write += record_split.cache_write;
        split->out += record_split.out;
        has_priced_record = 1;
    }
    return has_priced_record;
}

void agent_spend_free(struct spend_totals *totals)
{
    for (size_t i = 0; i < totals->count; i++) {
        free(totals->records[i].catalog_id);
        free(totals->records[i].model);
    }
    free(totals->records);
    memset(totals, 0, sizeof(*totals));
}

/* Most providers bill cache writes instead of input; unknown rates use that common policy. */
static long default_uncached_input(const struct stream_usage *usage)
{
    long cached = usage->cached_tokens > 0 ? usage->cached_tokens : 0;
    long cache_write = usage->cache_write_tokens > 0 ? usage->cache_write_tokens : 0;
    long input = usage->input_tokens > 0 ? usage->input_tokens : 0;
    long uncached = input - cached - cache_write;
    return uncached > 0 ? uncached : 0;
}

long agent_usage_uncached_input(const struct stream_usage *usage, const struct provider *provider,
                                const char *model)
{
    struct catalog_entry rates;
    struct catalog_split split;
    if (!model_meta_rates(provider, model, &rates) ||
        catalog_price(&rates, usage->input_tokens, usage->output_tokens, usage->cached_tokens,
                      usage->cache_write_tokens, usage->cache_write_1h_tokens, &split) < 0)
        return default_uncached_input(usage);
    return split.in_tokens;
}

int agent_usage_is_reported(const struct stream_usage *usage)
{
    return usage->input_tokens >= 0 || usage->output_tokens >= 0 || usage->cost >= 0;
}

struct turn_usage *agent_turn_usage_new(const struct stream_usage *usage, long elapsed_ms,
                                        const struct provider *provider, const char *model)
{
    if (!agent_usage_is_reported(usage) && elapsed_ms < 0)
        return NULL;

    struct turn_usage *turn_usage = xmalloc(sizeof(*turn_usage));
    turn_usage->usage = *usage;
    turn_usage->elapsed_ms = elapsed_ms;
    turn_usage->cost_input = -1;
    turn_usage->cost_cache_read = -1;
    turn_usage->cost_cache_write = -1;
    turn_usage->cost_output = -1;
    turn_usage->cost_total = usage->cost;
    turn_usage->cost_estimated = 0;
    turn_usage->uncached_input_tokens = default_uncached_input(usage);

    /* Without token counts, zero-valued categories would imply a decomposition we do not know. */
    if (usage->input_tokens < 0 && usage->output_tokens < 0)
        return turn_usage;
    if (!model || !*model)
        return turn_usage;

    struct catalog_entry rates;
    if (!model_meta_rates(provider, model, &rates))
        return turn_usage;

    struct catalog_split split;
    double total =
        catalog_price(&rates, usage->input_tokens, usage->output_tokens, usage->cached_tokens,
                      usage->cache_write_tokens, usage->cache_write_1h_tokens, &split);
    if (total < 0)
        return turn_usage;

    turn_usage->uncached_input_tokens = split.in_tokens;
    turn_usage->cost_input = split.in;
    turn_usage->cost_cache_read = split.cache_read;
    turn_usage->cost_cache_write = split.cache_write;
    turn_usage->cost_output = split.out;
    if (turn_usage->cost_total < 0) {
        turn_usage->cost_total = total;
        turn_usage->cost_estimated = 1;
    }
    return turn_usage;
}
