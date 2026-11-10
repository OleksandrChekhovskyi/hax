/* SPDX-License-Identifier: MIT */
#include "agent_stats.h"

#include <stdio.h>
#include <string.h>

#include "agent_core.h"
#include "catalog.h"
#include "model_meta.h"
#include "provider.h"
#include "providers/registry.h"
#include "text/fmt.h"

static int strings_equal(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return strcmp(a, b) == 0;
}

/* Usage records written before the catalog answered carry no price; a record's own provider and
 * model still identify the rates once they are known. */
static int usage_rates(const struct item *item, const struct provider *provider,
                       struct catalog_entry *rates)
{
    if (!item->model || !*item->model)
        return 0;
    if (provider && strings_equal(provider_stable_id(provider), item->provider))
        return model_meta_rates(provider, item->model, rates);
    const struct provider_def *def = item->provider ? provider_find(item->provider) : NULL;
    return catalog_lookup(item->provider, def ? provider_catalog_id(def) : NULL, item->model,
                          rates) == 0 &&
           rates->cost_input >= 0 && rates->cost_output >= 0;
}

static void add_reported(long *sum, long value)
{
    if (value > 0)
        *sum += value;
}

static void account_usage(struct agent_stats_totals *totals, const struct item *item,
                          const struct provider *provider)
{
    const struct turn_usage *record = item->usage;
    const struct stream_usage *usage = &record->usage;

    totals->requests++;
    add_reported(&totals->input_tokens, usage->input_tokens);
    add_reported(&totals->output_tokens, usage->output_tokens);
    add_reported(&totals->cached_tokens, usage->cached_tokens);
    add_reported(&totals->cache_write_tokens, usage->cache_write_tokens);
    add_reported(&totals->uncached_input_tokens, record->uncached_input_tokens);

    /* Nothing reported, nothing to price: a duration-only record neither estimates nor waits
     * for rates that could never apply to it. */
    if (usage->input_tokens < 0 && usage->output_tokens < 0 && record->cost_total < 0)
        return;

    double total = record->cost_total;
    int estimated = record->cost_estimated;
    struct catalog_split split = {
        .cost_input = record->cost_input,
        .cost_cache_read = record->cost_cache_read,
        .cost_cache_write = record->cost_cache_write,
        .cost_output = record->cost_output,
    };
    int split_known = record->cost_input >= 0 && record->cost_output >= 0;

    struct catalog_entry rates;
    if ((total < 0 || !split_known) && (usage->input_tokens >= 0 || usage->output_tokens >= 0) &&
        usage_rates(item, provider, &rates)) {
        struct catalog_split priced;
        double estimate =
            catalog_price(&rates, usage->input_tokens, usage->output_tokens, usage->cached_tokens,
                          usage->cache_write_tokens, usage->cache_write_1h_tokens, &priced);
        if (estimate >= 0) {
            split = priced;
            split_known = 1;
            if (total < 0) {
                total = estimate;
                estimated = 1;
            }
        }
    }

    if (total >= 0) {
        totals->spend += total;
        if (estimated)
            totals->spend_estimated = 1;
    } else {
        totals->spend_estimated = 1;
        totals->unpriced = 1;
    }
    if (split_known) {
        totals->split.cost_input += split.cost_input;
        totals->split.cost_cache_read += split.cost_cache_read;
        totals->split.cost_cache_write += split.cost_cache_write;
        totals->split.cost_output += split.cost_output;
        totals->split_available = 1;
    }
}

static struct agent_stats_totals *model_bucket(struct agent_stats *stats, const struct item *item)
{
    const struct turn_provenance *provenance = &item->usage->provenance;
    const char *provider = provenance->provider_label ? provenance->provider_label : item->provider;
    const char *model = provenance->model_label ? provenance->model_label : item->model;

    for (size_t i = 0; i < stats->n_models; i++)
        if (strings_equal(stats->models[i].provider, provider) &&
            strings_equal(stats->models[i].model, model))
            return &stats->models[i].totals;
    if (stats->n_models == AGENT_STATS_MAX_MODELS)
        return NULL;
    struct agent_stats_model *entry = &stats->models[stats->n_models++];
    entry->provider = provider;
    entry->model = model;
    return &entry->totals;
}

static void count_tool_call(struct agent_stats *stats, const char *name)
{
    stats->tool_calls++;
    if (!name)
        return;
    for (size_t i = 0; i < AGENT_STATS_MAX_TOOLS; i++) {
        if (stats->tools[i].name && strcmp(stats->tools[i].name, name) == 0) {
            stats->tools[i].count++;
            return;
        }
        if (!stats->tools[i].name) {
            stats->tools[i].name = name;
            stats->tools[i].count = 1;
            return;
        }
    }
}

static void account_item(struct agent_stats *stats, const struct item *item,
                         const struct provider *provider, int live)
{
    if (item->inherited) {
        /* Retired inherited items are neither this session's context nor its bill. */
        if (!live)
            return;
        if (item_is_typed_prompt(item))
            stats->inherited_user_turns++;
        else if (item->kind == ITEM_TURN_USAGE && item->usage)
            account_usage(&stats->inherited, item, provider);
        return;
    }

    /* Structure describes the live conversation; only the bill covers undone work. */
    if (item_is_typed_prompt(item)) {
        if (live)
            stats->user_turns++;
        else
            stats->undone_user_turns++;
    } else if (item->kind == ITEM_TOOL_CALL) {
        if (live)
            count_tool_call(stats, item->tool_name);
    } else if (item->kind == ITEM_TURN_USAGE && item->usage) {
        account_usage(&stats->total, item, provider);
        struct agent_stats_totals *bucket = model_bucket(stats, item);
        if (bucket)
            account_usage(bucket, item, provider);
    }
}

void agent_stats_collect(const struct agent_session *session, size_t from_item, size_t from_retired,
                         const struct provider *provider, struct agent_stats *out)
{
    memset(out, 0, sizeof(*out));
    for (size_t i = from_item; i < session->n_items; i++)
        account_item(out, &session->items[i], provider, 1);
    for (size_t i = from_retired; i < session->n_retired; i++)
        account_item(out, &session->retired[i], provider, 0);
    out->worked_ms = session->worked_ms;
    out->context_tokens = agent_session_last_context_tokens(session);
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
