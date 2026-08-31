/* SPDX-License-Identifier: MIT */
#include "agent_usage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "model_meta.h"
#include "provider.h"
#include "xalloc.h"
#include "text/fmt.h"

struct spend_record {
    struct stream_usage usage;
    double reported_cost; /* -1 when the provider did not report cost */
    struct catalog_entry rates;
    int has_rates;
    char *provider_id;
    char *catalog_id;
    char *model;
};

/* "2.0k" reads as noise next to "412" and "2.5k"; print whole multiples bare. */
static void format_one_decimal(char *out, size_t out_size, double value, char suffix)
{
    snprintf(out, out_size, "%.1f%c", value, suffix);
    char *zero_fraction = strstr(out, ".0");
    if (zero_fraction)
        memmove(zero_fraction, zero_fraction + 2, strlen(zero_fraction + 2) + 1);
}

void format_tokens(char *out, size_t out_size, long tokens)
{
    const long million = 1000000L;
    if (tokens < 0)
        snprintf(out, out_size, "?");
    else if (tokens < 1000)
        snprintf(out, out_size, "%ld", tokens);
    else if (tokens < 10L * 1000)
        format_one_decimal(out, out_size, (double)tokens / 1000.0, 'k');
    else if (tokens < million)
        snprintf(out, out_size, "%ldk", tokens / 1000 + (tokens % 1000 >= 500));
    else if (tokens < 10L * million)
        format_one_decimal(out, out_size, (double)tokens / (double)million, 'M');
    else
        snprintf(out, out_size, "%ldM", tokens / million + (tokens % million >= million / 2));
}

void format_context(char *out, size_t out_size, long context_tokens, long context_limit)
{
    char used[32];
    format_tokens(used, sizeof(used), context_tokens);
    if (context_limit > 0 && context_tokens >= 0) {
        char limit[32];
        /* Usage above the window is real (stale model metadata), so report it rather than
         * capping at 100; the ceiling only keeps the field three digits wide. */
        double ratio = (double)context_tokens * 100.0 / (double)context_limit;
        long percentage = ratio > 999.0 ? 999 : (long)ratio;
        format_tokens(limit, sizeof(limit), context_limit);
        snprintf(out, out_size, "%s / %s (%ld%%)", used, limit, percentage);
    } else if (context_limit > 0) {
        char limit[32];
        format_tokens(limit, sizeof(limit), context_limit);
        snprintf(out, out_size, "%s / %s", used, limit);
    } else {
        snprintf(out, out_size, "%s", used);
    }
}

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
    const char *provider_id = provider ? provider_stable_id(provider) : NULL;
    record->provider_id = provider_id && *provider_id ? xstrdup(provider_id) : NULL;
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
    if ((!record->provider_id && !record->catalog_id) || !record->model)
        return -1;

    struct catalog_entry rates;
    if (catalog_lookup(record->provider_id, record->catalog_id, record->model, &rates) != 0)
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
        split->cost_input += record_split.cost_input;
        split->cost_cache_read += record_split.cost_cache_read;
        split->cost_cache_write += record_split.cost_cache_write;
        split->cost_output += record_split.cost_output;
        has_priced_record = 1;
    }
    return has_priced_record;
}

void agent_spend_free(struct spend_totals *totals)
{
    for (size_t i = 0; i < totals->count; i++) {
        free(totals->records[i].provider_id);
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
    return split.uncached_input_tokens;
}

int agent_usage_is_reported(const struct stream_usage *usage)
{
    return usage->input_tokens >= 0 || usage->output_tokens >= 0 || usage->cost >= 0;
}

static long add_reported(long sum, long extra)
{
    if (extra < 0)
        return sum;
    return (sum < 0 ? 0 : sum) + extra;
}

static int has_unpriced_tokens(const struct stream_usage *usage)
{
    return usage->cost < 0 && (usage->input_tokens >= 0 || usage->output_tokens >= 0);
}

void agent_usage_add(struct stream_usage *sum, const struct stream_usage *extra)
{
    /* Evaluate before the token fields merge below. */
    int unpriced = has_unpriced_tokens(sum) || has_unpriced_tokens(extra);

    sum->input_tokens = add_reported(sum->input_tokens, extra->input_tokens);
    sum->output_tokens = add_reported(sum->output_tokens, extra->output_tokens);
    sum->cached_tokens = add_reported(sum->cached_tokens, extra->cached_tokens);
    sum->cache_write_tokens = add_reported(sum->cache_write_tokens, extra->cache_write_tokens);
    sum->cache_write_1h_tokens =
        add_reported(sum->cache_write_1h_tokens, extra->cache_write_1h_tokens);

    /* An exact cost must cover every token it is summed with; tokens reported without cost
     * make the aggregate unpriceable, so it falls back to the estimated path instead of
     * underreporting an "exact" charge. */
    if (unpriced)
        sum->cost = -1;
    else if (extra->cost >= 0)
        sum->cost = (sum->cost < 0 ? 0 : sum->cost) + extra->cost;
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
    turn_usage->provenance = (struct turn_provenance){0};

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

    turn_usage->uncached_input_tokens = split.uncached_input_tokens;
    turn_usage->cost_input = split.cost_input;
    turn_usage->cost_cache_read = split.cost_cache_read;
    turn_usage->cost_cache_write = split.cost_cache_write;
    turn_usage->cost_output = split.cost_output;
    if (turn_usage->cost_total < 0) {
        turn_usage->cost_total = total;
        turn_usage->cost_estimated = 1;
    }
    return turn_usage;
}
