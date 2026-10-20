/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "agent_usage.h"
#include "harness.h"
#include "model_meta.h"
#include "provider.h"

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

static void test_attempt_log_records_served_attempts(void)
{
    struct attempt_log log;
    attempt_log_init(&log);
    EXPECT(!attempt_log_has_usage(&log));

    /* A rejected retry that reported nothing served no request. */
    attempt_log_retry(&log, NULL, 0);
    struct stream_usage silent = usage(-1, -1, -1, -1);
    attempt_log_retry(&log, &silent, 0);
    EXPECT(log.count == 0);

    struct stream_usage dead = usage(40, -1, -1, -1);
    attempt_log_retry(&log, &dead, 0);
    struct stream_usage done = usage(100, 20, -1, 0.01);
    struct stream_response response = {.id = "r1", .model = "served", .route = "edge"};
    attempt_log_record(&log, &done, &response);

    EXPECT(log.count == 2);
    EXPECT(attempt_log_has_usage(&log));
    EXPECT(log.attempts[0].usage.input_tokens == 40);
    EXPECT(log.attempts[0].response_id == NULL);
    EXPECT(log.attempts[1].usage.input_tokens == 100);
    EXPECT_STR_EQ(log.attempts[1].response_id, "r1");
    EXPECT_STR_EQ(log.attempts[1].served_model, "served");
    EXPECT_STR_EQ(log.attempts[1].route, "edge");
    EXPECT(log.attempts[0].elapsed_ms >= 0 && log.attempts[1].elapsed_ms >= 0);
    attempt_log_free(&log);
    EXPECT(log.count == 0);
}

/* The backoff between attempts is nobody's request time. */
static void test_attempt_log_excludes_retry_backoff(void)
{
    struct attempt_log log;
    attempt_log_init(&log);
    struct stream_usage dead = usage(40, -1, -1, -1);
    attempt_log_retry(&log, &dead, 5000);
    struct stream_usage done = usage(100, 20, -1, -1);
    attempt_log_record(&log, &done, NULL);
    EXPECT(log.count == 2);
    /* Recorded immediately, with the 5s sleep never taken: the attempt's own time is nothing,
     * not the backoff. */
    EXPECT(log.attempts[1].elapsed_ms == 0);
    attempt_log_free(&log);
}

static void test_attempt_log_folds_overflow_into_last(void)
{
    struct attempt_log log;
    attempt_log_init(&log);
    struct stream_usage one = usage(1, 1, -1, -1);
    for (size_t i = 0; i < ATTEMPT_LOG_MAX + 2; i++)
        attempt_log_record(&log, &one, NULL);
    EXPECT(log.count == ATTEMPT_LOG_MAX);
    EXPECT(log.attempts[ATTEMPT_LOG_MAX - 1].usage.input_tokens == 3);
    attempt_log_free(&log);
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
    turn_usage_free(turn_usage);
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
        turn_usage_free(turn_usage);
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
    turn_usage_free(turn_usage);
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
    turn_usage_free(turn_usage);
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
    turn_usage_free(turn_usage);
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

int main(void)
{
    install_catalog();
    test_attempt_log_records_served_attempts();
    test_attempt_log_excludes_retry_backoff();
    test_attempt_log_folds_overflow_into_last();
    test_turn_usage_with_reported_cost();
    test_turn_usage_with_duration_only();
    test_turn_usage_with_cost_but_no_tokens();
    test_turn_usage_with_estimated_cost();
    test_turn_usage_without_rates();
    test_usage_add_keeps_unreported_sentinels();
    test_usage_add_drops_cost_over_unpriced_tokens();

    T_REPORT();
}
