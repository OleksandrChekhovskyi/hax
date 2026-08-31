/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "agent_usage.h"
#include "catalog.h"
#include "harness.h"
#include "model_meta.h"
#include "provider.h"
#include "xalloc.h"

static const struct provider CATALOG_PROVIDER = {.catalog_id = "prov"};

/* Catalog misses are memoized, so install the fixture before any pricing call. */
static void install_catalog(void)
{
    char *dir = t_tempdir();
    setenv("XDG_CACHE_HOME", dir, 1);

    char path[600];
    snprintf(path, sizeof(path), "%s/hax", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/hax/catalog.json", dir);
    FILE *file = fopen(path, "w");
    EXPECT(file != NULL);
    if (!file)
        return;
    fputs("{\"prov\": {\"models\": {"
          "\"m\": {\"cost\": {\"input\": 2, \"output\": 8}},"
          "\"free-m\": {\"cost\": {\"input\": 0, \"output\": 0}}"
          "}}}",
          file);
    fclose(file);
}

static struct stream_usage usage(long input, long output, long cached, double cost)
{
    return (struct stream_usage){
        .input_tokens = input,
        .output_tokens = output,
        .cached_tokens = cached,
        .cache_write_tokens = -1,
        .cache_write_1h_tokens = -1,
        .cost = cost,
    };
}

static void test_format_stats_segments(void)
{
    char segments[AGENT_STATS_MAX_SEGMENTS][AGENT_STATS_SEGMENT_LEN];

    int count = agent_format_stats_segments(segments, 9113, 262144, 42000, 0.042, 0);
    EXPECT(count == 3);
    EXPECT_STR_EQ(segments[0], "42s");
    EXPECT_STR_EQ(segments[1], "9.1k / 262k (3%)");
    EXPECT_STR_EQ(segments[2], "$0.042");

    count = agent_format_stats_segments(segments, 9113, 0, 42000, 0.042, 0);
    EXPECT(count == 3);
    EXPECT_STR_EQ(segments[1], "context 9.1k");

    count = agent_format_stats_segments(segments, -1, 0, -1, 0.042, 1);
    EXPECT(count == 1);
    EXPECT_STR_EQ(segments[0], "~$0.042");

    count = agent_format_stats_segments(segments, -1, 0, 42000, 0, 0);
    EXPECT(count == 1);
    EXPECT_STR_EQ(segments[0], "42s");

    EXPECT(agent_format_stats_segments(segments, -1, 0, -1, 0, 0) == 0);
}

static void test_reported_spend_is_exact(void)
{
    struct spend_totals totals = {0};
    struct stream_usage reported = usage(1000, 50, 200, 0.01);
    agent_spend_account(&totals, &reported, &CATALOG_PROVIDER, "m");

    int estimated = 1;
    EXPECT(agent_spend_total(&totals, &estimated) == 0.01);
    EXPECT(!estimated);

    struct catalog_split split;
    EXPECT(agent_spend_split(&totals, &split));
    EXPECT(split.cost_input == 800 * 2.0 / 1e6);
    EXPECT(split.cost_cache_read == 200 * 2.0 / 1e6);
    EXPECT(split.cost_output == 50 * 8.0 / 1e6);

    agent_spend_free(&totals);
}

static void test_unpriced_spend_is_approximate(void)
{
    struct spend_totals totals = {0};
    struct provider unknown_provider = {.catalog_id = "unknown"};
    struct provider anonymous_provider = {0};
    struct stream_usage reported = usage(1000, 50, 200, 0.01);
    struct stream_usage unreported = reported;
    unreported.cost = -1;

    agent_spend_account(&totals, &reported, &CATALOG_PROVIDER, "m");
    agent_spend_account(&totals, &unreported, &unknown_provider, "m");
    agent_spend_account(&totals, &unreported, &anonymous_provider, NULL);

    int estimated = 0;
    EXPECT(agent_spend_has_unpriced(&totals));
    EXPECT(agent_spend_total(&totals, &estimated) == 0.01);
    EXPECT(estimated);

    agent_spend_free(&totals);
}

static void test_spend_ignores_empty_usage(void)
{
    struct spend_totals totals = {0};
    struct stream_usage empty = {-1, -1, -1, -1, -1, -1};
    agent_spend_account(&totals, &empty, &CATALOG_PROVIDER, "m");
    EXPECT(totals.count == 0);
    agent_spend_free(&totals);
}

static void test_reported_zero_spend_is_exact(void)
{
    struct spend_totals totals = {0};
    struct stream_usage reported = usage(1000, 50, 200, 0);
    agent_spend_account(&totals, &reported, &CATALOG_PROVIDER, "m");

    int estimated = 1;
    EXPECT(agent_spend_total(&totals, &estimated) == 0);
    EXPECT(!estimated);
    agent_spend_free(&totals);
}

static void test_catalog_spend_estimate(void)
{
    struct stream_usage unreported = usage(1000000, 1000000, -1, -1);
    struct spend_totals priced = {0};
    agent_spend_account(&priced, &unreported, &CATALOG_PROVIDER, "m");

    int estimated = 0;
    EXPECT(agent_spend_total(&priced, &estimated) == 10.0);
    EXPECT(estimated);
    EXPECT(!agent_spend_has_unpriced(&priced));

    struct catalog_split split;
    EXPECT(agent_spend_split(&priced, &split));
    EXPECT(split.cost_input == 2.0 && split.cost_output == 8.0);
    EXPECT(split.cost_cache_read == 0 && split.cost_cache_write == 0);

    struct spend_totals missing = {0};
    agent_spend_account(&missing, &unreported, &CATALOG_PROVIDER, "unknown-model");
    EXPECT(agent_spend_total(&missing, &estimated) == 0);
    EXPECT(estimated);
    EXPECT(agent_spend_has_unpriced(&missing));
    EXPECT(!agent_spend_split(&missing, &split));

    agent_spend_free(&priced);
    agent_spend_free(&missing);
}

static void test_zero_rate_estimate_remains_approximate(void)
{
    struct spend_totals totals = {0};
    struct stream_usage paid = usage(1000000, 1000000, -1, 0.5);
    struct stream_usage free_usage = paid;
    free_usage.cost = -1;
    agent_spend_account(&totals, &paid, &CATALOG_PROVIDER, "m");
    agent_spend_account(&totals, &free_usage, &CATALOG_PROVIDER, "free-m");

    int estimated = 0;
    EXPECT(agent_spend_total(&totals, &estimated) == 0.5);
    EXPECT(estimated);
    EXPECT(!agent_spend_has_unpriced(&totals));
    agent_spend_free(&totals);
}

static void publish_rates(struct provider *provider, const char *model, double input, double output)
{
    struct model_info info;
    model_info_init(&info);
    info.id = xstrdup(model);
    info.cost_input = input;
    info.cost_output = output;
    model_meta_store(provider, &info);
    model_info_clear(&info);
}

static void test_spend_snapshots_live_rates(void)
{
    struct provider provider = {0};
    struct spend_totals totals = {0};
    struct stream_usage unreported = usage(1000000, 1000000, -1, -1);

    publish_rates(&provider, "cheap", 1, 2);
    agent_spend_account(&totals, &unreported, &provider, "cheap");
    publish_rates(&provider, "dear", 100, 200);
    agent_spend_account(&totals, &unreported, &provider, "dear");

    int estimated = 0;
    EXPECT(agent_spend_total(&totals, &estimated) == 303.0);
    EXPECT(estimated);
    struct catalog_split split;
    EXPECT(agent_spend_split(&totals, &split));
    EXPECT(split.cost_input == 101.0 && split.cost_output == 202.0);

    agent_spend_free(&totals);
    model_meta_release(&provider);
}

static void test_turn_usage_with_reported_cost(void)
{
    struct stream_usage reported = usage(1000, 50, 200, 0.01);
    struct turn_usage *turn_usage = agent_turn_usage_new(&reported, 1500, &CATALOG_PROVIDER, "m");
    EXPECT(turn_usage != NULL);
    if (!turn_usage)
        return;

    EXPECT(turn_usage->cost_total == 0.01);
    EXPECT(!turn_usage->cost_estimated);
    EXPECT(turn_usage->cost_input == 800 * 2.0 / 1e6);
    EXPECT(turn_usage->cost_cache_read == 200 * 2.0 / 1e6);
    EXPECT(turn_usage->cost_output == 50 * 8.0 / 1e6);
    EXPECT(turn_usage->elapsed_ms == 1500);
    free(turn_usage);
}

static void test_turn_usage_with_duration_only(void)
{
    struct stream_usage empty = {-1, -1, -1, -1, -1, -1};
    EXPECT(!agent_usage_is_reported(&empty));

    struct turn_usage *turn_usage = agent_turn_usage_new(&empty, 1500, &CATALOG_PROVIDER, "m");
    EXPECT(turn_usage != NULL);
    if (turn_usage) {
        EXPECT(turn_usage->elapsed_ms == 1500);
        EXPECT(turn_usage->cost_total < 0);
        EXPECT(!turn_usage->cost_estimated);
        EXPECT(turn_usage->cost_input < 0 && turn_usage->cost_output < 0);
        free(turn_usage);
    }
    EXPECT(agent_turn_usage_new(&empty, -1, &CATALOG_PROVIDER, "m") == NULL);
}

static void test_turn_usage_with_cost_but_no_tokens(void)
{
    struct stream_usage reported = {-1, -1, -1, -1, -1, 0.02};
    struct turn_usage *turn_usage = agent_turn_usage_new(&reported, -1, &CATALOG_PROVIDER, "m");
    EXPECT(turn_usage != NULL);
    if (!turn_usage)
        return;

    EXPECT(turn_usage->cost_total == 0.02);
    EXPECT(!turn_usage->cost_estimated);
    EXPECT(turn_usage->cost_input < 0 && turn_usage->cost_output < 0);
    free(turn_usage);
}

static void test_turn_usage_with_estimated_cost(void)
{
    struct stream_usage unreported = usage(1000000, 1000000, 500000, -1);
    struct turn_usage *turn_usage = agent_turn_usage_new(&unreported, -1, &CATALOG_PROVIDER, "m");
    EXPECT(turn_usage != NULL);
    if (!turn_usage)
        return;

    EXPECT(turn_usage->cost_estimated);
    EXPECT(turn_usage->cost_total == 10.0);
    EXPECT(turn_usage->cost_input == 1.0);
    EXPECT(turn_usage->cost_cache_read == 1.0);
    EXPECT(turn_usage->cost_cache_write == 0);
    EXPECT(turn_usage->cost_output == 8.0);
    free(turn_usage);
}

static void test_turn_usage_without_rates(void)
{
    struct stream_usage unreported = usage(1000000, 1000000, 500000, -1);
    struct turn_usage *turn_usage = agent_turn_usage_new(&unreported, 2000, NULL, NULL);
    EXPECT(turn_usage != NULL);
    if (!turn_usage)
        return;

    EXPECT(turn_usage->cost_total < 0);
    EXPECT(!turn_usage->cost_estimated);
    EXPECT(turn_usage->usage.input_tokens == 1000000);
    free(turn_usage);
}

static void test_usage_add_keeps_unreported_sentinels(void)
{
    struct stream_usage sum = usage(-1, -1, -1, -1);
    struct stream_usage extra = usage(40, -1, 5, -1);
    agent_usage_add(&sum, &extra);
    EXPECT(sum.input_tokens == 40);
    EXPECT(sum.output_tokens == -1);
    EXPECT(sum.cached_tokens == 5);
    EXPECT(sum.cost < 0);

    agent_usage_add(&sum, &extra);
    EXPECT(sum.input_tokens == 80);
}

static void test_usage_add_drops_cost_over_unpriced_tokens(void)
{
    /* Token-only extra: the exact cost no longer covers the aggregate's tokens. */
    struct stream_usage sum = usage(100, 20, -1, 0.5);
    agent_usage_add(&sum, &(struct stream_usage){.input_tokens = 40,
                                                 .output_tokens = -1,
                                                 .cached_tokens = -1,
                                                 .cache_write_tokens = -1,
                                                 .cache_write_1h_tokens = -1,
                                                 .cost = -1});
    EXPECT(sum.input_tokens == 140);
    EXPECT(sum.cost < 0);

    /* Both sides priced: charges sum and stay exact. */
    sum = usage(100, 20, -1, 0.5);
    struct stream_usage priced = usage(40, 5, -1, 0.25);
    agent_usage_add(&sum, &priced);
    EXPECT(sum.input_tokens == 140);
    EXPECT(sum.cost == 0.75);

    /* A no-op merge cannot invalidate a priced side. */
    struct stream_usage empty = usage(-1, -1, -1, -1);
    agent_usage_add(&sum, &empty);
    EXPECT(sum.cost == 0.75);

    /* A charge merged over unpriced tokens is equally uncoverable. */
    sum = usage(80, 10, -1, -1);
    struct stream_usage cost_only = usage(-1, -1, -1, 0.25);
    agent_usage_add(&sum, &cost_only);
    EXPECT(sum.input_tokens == 80);
    EXPECT(sum.cost < 0);
}

/* ---------- format_tokens / format_context ---------- */

static void test_format_tokens_ranges(void)
{
    char buf[32];
    format_tokens(buf, sizeof(buf), -1);
    EXPECT_STR_EQ(buf, "?");
    format_tokens(buf, sizeof(buf), 412);
    EXPECT_STR_EQ(buf, "412");
    format_tokens(buf, sizeof(buf), 5410);
    EXPECT_STR_EQ(buf, "5.4k");
    format_tokens(buf, sizeof(buf), 2000); /* whole multiples print bare */
    EXPECT_STR_EQ(buf, "2k");
    format_tokens(buf, sizeof(buf), 262144); /* decimal suffixes even for binary windows */
    EXPECT_STR_EQ(buf, "262k");
    format_tokens(buf, sizeof(buf), 872000);
    EXPECT_STR_EQ(buf, "872k");
    format_tokens(buf, sizeof(buf), 1000000);
    EXPECT_STR_EQ(buf, "1M");
    format_tokens(buf, sizeof(buf), 1200000);
    EXPECT_STR_EQ(buf, "1.2M");
    format_tokens(buf, sizeof(buf), 12000000);
    EXPECT_STR_EQ(buf, "12M");
}

static void test_format_context_with_and_without_limit(void)
{
    char buf[64];
    format_context(buf, sizeof(buf), 9113, 262144);
    EXPECT_STR_EQ(buf, "9.1k / 262k (3%)");
    format_context(buf, sizeof(buf), 9113, 0); /* unknown window */
    EXPECT_STR_EQ(buf, "9.1k");
    format_context(buf, sizeof(buf), 300000, 262144); /* stale window metadata reports over 100% */
    EXPECT_STR_EQ(buf, "300k / 262k (114%)");
    format_context(buf, sizeof(buf), -1, 262144); /* known window, no usage reported yet */
    EXPECT_STR_EQ(buf, "? / 262k");
    format_context(buf, sizeof(buf), -1, 0); /* nothing known */
    EXPECT_STR_EQ(buf, "?");
}

static void test_format_usage_extremes(void)
{
    char formatted[64];
    format_tokens(formatted, sizeof(formatted), LONG_MAX);
    EXPECT(formatted[0] != '-');

    format_context(formatted, sizeof(formatted), LONG_MAX, 1);
    EXPECT(strstr(formatted, "(999%)") != NULL);
}

int main(void)
{
    install_catalog();
    test_format_stats_segments();
    test_reported_spend_is_exact();
    test_unpriced_spend_is_approximate();
    test_spend_ignores_empty_usage();
    test_reported_zero_spend_is_exact();
    test_catalog_spend_estimate();
    test_zero_rate_estimate_remains_approximate();
    test_spend_snapshots_live_rates();
    test_turn_usage_with_reported_cost();
    test_turn_usage_with_duration_only();
    test_turn_usage_with_cost_but_no_tokens();
    test_turn_usage_with_estimated_cost();
    test_turn_usage_without_rates();
    test_usage_add_keeps_unreported_sentinels();
    test_usage_add_drops_cost_over_unpriced_tokens();

    test_format_tokens_ranges();
    test_format_context_with_and_without_limit();
    test_format_usage_extremes();

    T_REPORT();
}
