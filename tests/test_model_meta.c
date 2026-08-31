/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "catalog.h"
#include "config.h"
#include "effort.h"
#include "harness.h"
#include "model_meta.h"
#include "provider.h"
#include "xalloc.h"
#include "providers/registry.h"

static void write_catalog_fixture(void)
{
    char *dir = t_tempdir();
    setenv("XDG_CACHE_HOME", dir, 1);
    char path[600];
    snprintf(path, sizeof(path), "%s/hax", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/hax/catalog.json", dir);
    FILE *f = fopen(path, "w");
    EXPECT(f != NULL);
    if (f) {
        fputs("{\"openai\": {\"models\": {\"m\": {\"limit\": {\"context\": 64000},"
              "\"modalities\": {\"input\": [\"text\", \"image\"]}},"
              "\"priced\": {\"cost\": {\"input\": 2, \"output\": 8, \"cache_read\": 0.5,"
              " \"tiers\": [{\"tier\": {\"type\": \"context\", \"size\": 200000},"
              " \"input\": 4, \"output\": 16}]}},"
              "\"foreign-ladder\": {\"reasoning_options\":"
              " [{\"type\": \"effort\", \"values\": [\"minimal\", \"low\", \"high\"]}]}}}}",
              f);
        fclose(f);
    }
}

static const char *const PROVIDER_LEVELS[] = {"none", "low", "medium", "high", "xhigh"};

static size_t list_provider_efforts(struct provider *p, const char *const **out)
{
    (void)p;
    *out = PROVIDER_LEVELS;
    return sizeof(PROVIDER_LEVELS) / sizeof(PROVIDER_LEVELS[0]);
}

static size_t list_no_efforts(struct provider *p, const char *const **out)
{
    (void)p;
    (void)out;
    return 0;
}

static struct provider make_provider(const char *name, size_t (*list_efforts)(struct provider *,
                                                                              const char *const **))
{
    struct provider provider = {
        .name = name,
        .list_efforts = list_efforts,
    };
    return provider;
}

static void store_efforts(struct provider *provider, const char *model, const char *const *levels)
{
    struct model_info info;
    model_info_init(&info);
    info.id = xstrdup(model);
    info.efforts.known = 1;
    for (size_t i = 0; levels && levels[i]; i++)
        effort_set_add(&info.efforts, levels[i]);
    model_meta_store(provider, &info);
    model_info_clear(&info);
}

static void test_rates_resolution(void)
{
    struct provider p = make_provider("x", NULL);
    struct catalog_entry entry;

    EXPECT(model_meta_rates(&p, "priced", &entry) == 0);
    EXPECT(entry.cost_input < 0 && entry.cost_output < 0);
    EXPECT(entry.cost_cache_write_1h < 0);
    EXPECT(entry.n_tiers == 0);

    p.catalog_id = "openai";
    EXPECT(model_meta_rates(&p, "priced", &entry) == 1);
    EXPECT(entry.cost_input == 2 && entry.cost_output == 8 && entry.cost_cache_read == 0.5);
    EXPECT(entry.n_tiers == 1 && entry.tiers[0].context_threshold == 200000);
    EXPECT(catalog_price(&entry, 1000000, 0, 0, 0, 0, NULL) == 4.0);

    struct model_info report;
    model_info_init(&report);
    report.id = xstrdup("priced");
    report.cost_input = 3;
    report.cost_output = 12;
    report.cost_cache_read = 0.3;
    report.cost_cache_write = 3.75;
    report.cost_cache_write_1h = 6;
    model_meta_store(&p, &report);
    EXPECT(model_meta_rates(&p, "priced", &entry) == 1);
    EXPECT(entry.cost_input == 3 && entry.cost_output == 12);
    EXPECT(entry.cost_cache_read == 0.3 && entry.cost_cache_write == 3.75);
    EXPECT(entry.cost_cache_write_1h == 6);
    /* Do not combine catalog thresholds with provider-reported rates. */
    EXPECT(entry.n_tiers == 0);
    EXPECT(catalog_price(&entry, 1000000, 0, 0, 0, 0, NULL) == 3.0);

    report.n_tiers = 1;
    report.tiers[0] = (struct catalog_tier){.context_threshold = 99999,
                                            .cost_input = 6,
                                            .cost_output = -1,
                                            .cost_cache_read = -1,
                                            .cost_cache_write = -1,
                                            .cost_cache_write_1h = -1};
    model_meta_store(&p, &report);
    EXPECT(model_meta_rates(&p, "priced", &entry) == 1);
    EXPECT(entry.n_tiers == 1 && entry.tiers[0].context_threshold == 99999);
    EXPECT(catalog_price(&entry, 1000000, 1000000, 0, 0, 0, NULL) == 18.0);

    model_info_clear(&report);
    model_meta_release(&p);
}

static void test_context_resolution(void)
{
    unsetenv("HAX_CONTEXT_LIMIT");
    struct provider p = make_provider("x", NULL);

    EXPECT(model_meta_context(&p, "m") == 0);

    p.catalog_id = "openai";
    EXPECT(model_meta_context(&p, "m") == 64000);
    EXPECT(model_meta_context(&p, NULL) == 0);
    EXPECT(model_meta_context(&p, "unknown-model") == 0);

    struct model_info report;
    model_info_init(&report);
    report.id = xstrdup("m");
    report.context = 32000;
    model_meta_store(&p, &report);
    model_info_clear(&report);
    EXPECT(model_meta_context(&p, "m") == 32000);
    EXPECT(model_meta_context(&p, "other") == 0);

    config_set_override("context_limit", "16k");
    EXPECT(model_meta_context(&p, "m") == 16000);
    config_set_override("context_limit", NULL);
    model_meta_release(&p);
}

static void test_image_input_resolution(void)
{
    unsetenv("HAX_IMAGE_INPUT");
    struct provider p = make_provider("x", NULL);
    EXPECT(model_meta_image_input(&p, "m") == -1);
    EXPECT(model_meta_image_input(NULL, NULL) == -1);

    p.catalog_id = "openai";
    EXPECT(model_meta_image_input(&p, "m") == 1);

    struct model_info report;
    model_info_init(&report);
    report.id = xstrdup("m");
    report.image_input = PROVIDER_CAP_NO;
    model_meta_store(&p, &report);
    model_info_clear(&report);
    EXPECT(model_meta_image_input(&p, "m") == 0);
    EXPECT(model_meta_image_input(&p, "other") == -1);

    setenv("HAX_IMAGE_INPUT", "on", 1);
    EXPECT(model_meta_image_input(&p, "m") == 1);
    setenv("HAX_IMAGE_INPUT", "auto", 1);
    EXPECT(model_meta_image_input(&p, "m") == 0);
    unsetenv("HAX_IMAGE_INPUT");
    model_meta_release(&p);
}

static int efforts_equal(const struct effort_set *actual, const char *const *expected)
{
    size_t expected_count = 0;
    while (expected[expected_count])
        expected_count++;
    if (!actual->known || actual->count != expected_count)
        return 0;
    for (size_t i = 0; i < expected_count; i++)
        if (strcmp(actual->values[i], expected[i]) != 0)
            return 0;
    return 1;
}

static void test_falls_back_to_provider_levels(void)
{
    struct provider p = make_provider("codex", list_provider_efforts);
    struct effort_set levels;
    model_meta_efforts(&p, "gpt-unknown", &levels);
    static const char *const expected[] = {"none", "low", "medium", "high", "xhigh", NULL};
    EXPECT(efforts_equal(&levels, expected));
    model_meta_release(&p);
}

static void test_provider_report_narrows(void)
{
    static const char *const reported[] = {"low", "medium", "high", NULL};
    struct provider p = make_provider("codex", list_provider_efforts);
    store_efforts(&p, "gpt-narrow", reported);
    struct effort_set levels;
    model_meta_efforts(&p, "gpt-narrow", &levels);
    static const char *const expected[] = {"low", "medium", "high", NULL};
    EXPECT(efforts_equal(&levels, expected));
    EXPECT(!effort_set_has(&levels, "none"));
    EXPECT(!effort_set_has(&levels, "xhigh"));
    model_meta_release(&p);
}

static void test_provider_report_extends_levels(void)
{
    static const char *const reported[] = {"max", "high", "medium", "low", NULL};
    struct provider p = make_provider("openrouter", list_provider_efforts);
    store_efforts(&p, "vendor/new", reported);
    struct effort_set levels;
    model_meta_efforts(&p, "vendor/new", &levels);
    static const char *const expected[] = {"low", "medium", "high", "max", NULL};
    EXPECT(efforts_equal(&levels, expected));
    model_meta_release(&p);
}

/* Catalog IDs may be shared by providers with different wire vocabularies. */
static void test_catalog_narrows_but_cannot_widen(void)
{
    struct provider p = make_provider("codex", list_provider_efforts);
    p.catalog_id = "openai";
    struct effort_set levels;
    model_meta_efforts(&p, "foreign-ladder", &levels);
    static const char *const expected[] = {"low", "high", NULL};
    EXPECT(efforts_equal(&levels, expected));
    EXPECT(!effort_set_has(&levels, "minimal"));
    model_meta_release(&p);
}

static void test_empty_report_removes_levels(void)
{
    struct provider p = make_provider("anthropic", list_provider_efforts);
    store_efforts(&p, "claude-budget", NULL);
    struct effort_set levels;
    model_meta_efforts(&p, "claude-budget", &levels);
    EXPECT(levels.known && levels.count == 0);
    model_meta_release(&p);
}

static void test_report_is_scoped_to_provider_and_model(void)
{
    static const char *const reported[] = {"low", NULL};
    struct provider provider = make_provider("codex", list_provider_efforts);
    struct provider other_provider = make_provider("openrouter", list_provider_efforts);
    store_efforts(&provider, "gpt-narrow", reported);
    struct effort_set levels;

    model_meta_efforts(&other_provider, "gpt-narrow", &levels);
    EXPECT(levels.count == 5);
    model_meta_efforts(&provider, "gpt-other", &levels);
    EXPECT(levels.count == 5);
    model_meta_efforts(&provider, "gpt-narrow", &levels);
    EXPECT(levels.count == 1);

    model_meta_release(&other_provider);
    model_meta_efforts(&provider, "gpt-narrow", &levels);
    EXPECT(levels.count == 1);
    model_meta_release(&provider);
}

static void test_snapshot_can_restore_report(void)
{
    static const char *const running[] = {"low", NULL};
    static const char *const candidate[] = {"low", "medium", "high", NULL};
    struct provider p = make_provider("openrouter", list_provider_efforts);
    store_efforts(&p, "model-running", running);

    struct model_info saved;
    EXPECT(model_meta_snapshot(&p, &saved) == 1);
    EXPECT_STR_EQ(saved.id, "model-running");

    store_efforts(&p, "model-candidate", candidate);
    struct effort_set levels;
    model_meta_efforts(&p, "model-running", &levels);
    EXPECT(levels.count == 5);

    model_meta_store(&p, &saved);
    model_info_clear(&saved);
    model_meta_efforts(&p, "model-running", &levels);
    EXPECT(levels.count == 1);
    model_meta_efforts(&p, "model-candidate", &levels);
    EXPECT(levels.count == 5);

    model_meta_release(&p);
    struct model_info empty;
    EXPECT(model_meta_snapshot(&p, &empty) == 0);
    model_info_clear(&empty);
}

static void test_id_only_report_is_ignored(void)
{
    unsetenv("HAX_CONTEXT_LIMIT");
    struct provider p = make_provider("llama.cpp", list_no_efforts);
    struct model_info bare;
    model_info_init(&bare);
    bare.id = xstrdup("qwen3.gguf");
    model_meta_store(&p, &bare);
    model_info_clear(&bare);

    struct model_info held;
    EXPECT(model_meta_snapshot(&p, &held) == 0);
    model_info_clear(&held);

    struct model_info props;
    model_info_init(&props);
    props.id = xstrdup("qwen3.gguf");
    props.context = 256000;
    model_meta_store(&p, &props);
    model_info_clear(&props);
    EXPECT(model_meta_context(&p, "qwen3.gguf") == 256000);
    model_meta_release(&p);
}

static int probe_calls;

static void parse_nothing(const char *body, const char *model, struct model_info *info)
{
    (void)body;
    (void)model;
    (void)info;
}

static int counting_probe(struct provider *provider, const char *model, struct model_probe *probe)
{
    (void)provider;
    (void)model;
    probe_calls++;
    /* Refused immediately; the probe result does not matter, only that it was attempted. */
    probe->url = xstrdup("http://127.0.0.1:1/props");
    probe->timeout_s = 1;
    probe->parse = parse_nothing;
    return 0;
}

static void store_report(struct provider *provider, const char *model, long context,
                         enum provider_cap image_input)
{
    struct model_info info;
    model_info_init(&info);
    info.id = xstrdup(model);
    info.context = context;
    info.image_input = image_input;
    model_meta_store(provider, &info);
    model_info_clear(&info);
}

/* A same-model store keeps previously known fields; a different model replaces the report. */
static void test_same_model_store_merges(void)
{
    unsetenv("HAX_IMAGE_INPUT");
    unsetenv("HAX_CONTEXT_LIMIT");
    struct provider p = make_provider("llama.cpp", list_no_efforts);
    store_report(&p, "m", 32000, PROVIDER_CAP_YES);
    store_report(&p, "m", 4096, PROVIDER_CAP_UNKNOWN);
    EXPECT(model_meta_context(&p, "m") == 4096);
    EXPECT(model_meta_image_input(&p, "m") == 1);

    store_report(&p, "other", 8192, PROVIDER_CAP_UNKNOWN);
    EXPECT(model_meta_context(&p, "m") == 0);
    EXPECT(model_meta_image_input(&p, "other") == -1);
    model_meta_release(&p);

    static const char *const reported[] = {"low", NULL};
    struct provider q = make_provider("openrouter", list_provider_efforts);
    store_efforts(&q, "m", reported);
    store_report(&q, "m", 4096, PROVIDER_CAP_UNKNOWN);
    struct effort_set levels;
    model_meta_efforts(&q, "m", &levels);
    EXPECT(levels.count == 1);
    EXPECT(model_meta_context(&q, "m") == 4096);
    model_meta_release(&q);
}

/* Explicit catalog.models configuration beats a live provider report, and a block scoped to the
 * runtime provider id does not leak onto other providers sharing the catalog identity. */
static void test_configured_context_beats_report(void)
{
    unsetenv("HAX_CONTEXT_LIMIT");
    EXPECT(config_load("{\"catalog\": {\"models\": {\"codex\": {"
                       "\"m\": {\"limit\": {\"context\": 872000}}}}}}") == 0);

    struct provider codex = make_provider("codex", NULL);
    codex.catalog_id = "openai";
    store_report(&codex, "m", 272000, PROVIDER_CAP_UNKNOWN);
    EXPECT(model_meta_context(&codex, "m") == 872000);

    struct provider openai = make_provider("openai", NULL);
    openai.catalog_id = "openai";
    store_report(&openai, "m", 272000, PROVIDER_CAP_UNKNOWN);
    EXPECT(model_meta_context(&openai, "m") == 272000);

    model_meta_release(&codex);
    model_meta_release(&openai);
    config_load(NULL);
}

/* Store a live report carrying base rates and, when `tier_threshold` is positive, one
 * long-context tier priced at `tier_input`. */
static void store_rates_report(struct provider *provider, const char *model, double cost_input,
                               double cost_output, long tier_threshold, double tier_input)
{
    struct model_info info;
    model_info_init(&info);
    info.id = xstrdup(model);
    info.cost_input = cost_input;
    info.cost_output = cost_output;
    if (tier_threshold > 0) {
        info.n_tiers = 1;
        info.tiers[0].context_threshold = tier_threshold;
        info.tiers[0].cost_input = tier_input;
        info.tiers[0].cost_output = -1;
        info.tiers[0].cost_cache_read = -1;
        info.tiers[0].cost_cache_write = -1;
        info.tiers[0].cost_cache_write_1h = -1;
    }
    model_meta_store(provider, &info);
    model_info_clear(&info);
}

static void test_configured_rates_beat_report(void)
{
    EXPECT(config_load("{\"catalog\": {\"models\": {\"codex\": {\"m\": {\"cost\": {\"input\": 5,"
                       " \"tiers\": [{\"input\": 10,"
                       " \"tier\": {\"type\": \"context\", \"size\": 100000}}]}}}}}}") == 0);

    struct provider p = make_provider("codex", NULL);
    store_rates_report(&p, "m", 3, 12, 0, 0);

    struct catalog_entry rates;
    EXPECT(model_meta_rates(&p, "m", &rates) == 1);
    EXPECT(rates.cost_input == 5);   /* configured beats the report */
    EXPECT(rates.cost_output == 12); /* the report fills what config omits */
    /* Configured tiers apply even alongside reported base rates. */
    EXPECT(rates.n_tiers == 1 && rates.tiers[0].cost_input == 10);

    model_meta_release(&p);
    config_load(NULL);
}

/* An explicitly empty tier list is the whole override: it must survive resolution and select
 * flat pricing over a tiered report. */
static void test_configured_empty_tiers_beat_tiered_report(void)
{
    EXPECT(config_load("{\"catalog\": {\"models\": {\"codex\": {\"m\": {"
                       "\"cost\": {\"tiers\": []}}}}}}") == 0);

    struct provider p = make_provider("codex", NULL);
    store_rates_report(&p, "m", 3, 12, 100000, 6);

    struct catalog_entry rates;
    EXPECT(model_meta_rates(&p, "m", &rates) == 1);
    EXPECT(rates.n_tiers == 0);
    EXPECT(catalog_price(&rates, 1000000, 0, 0, 0, 0, NULL) == 3.0);

    model_meta_release(&p);
    config_load(NULL);
}

static void test_configured_efforts_beat_report(void)
{
    EXPECT(config_load("{\"catalog\": {\"models\": {\"prov\": {\"m\": {"
                       "\"reasoning_options\": [{\"type\": \"effort\","
                       " \"values\": [\"low\", \"max\"]}]}}}}}") == 0);
    static const char *const reported[] = {"low", "medium", "high", NULL};
    struct provider p = make_provider("prov", list_provider_efforts);
    store_efforts(&p, "m", reported);
    struct effort_set levels;
    model_meta_efforts(&p, "m", &levels);
    /* Configured levels narrow the provider ladder and, like a report, may extend it. */
    static const char *const expected[] = {"low", "max", NULL};
    EXPECT(efforts_equal(&levels, expected));
    model_meta_release(&p);
    config_load(NULL);
}

/* A same-model refinement without the ceiling keeps the previously reported one. */
static void test_same_model_store_keeps_max_context(void)
{
    struct provider p = make_provider("codex", list_no_efforts);
    struct model_info info;
    model_info_init(&info);
    info.id = xstrdup("m");
    info.context = 272000;
    info.max_context = 872000;
    model_meta_store(&p, &info);
    model_info_clear(&info);

    store_report(&p, "m", 300000, PROVIDER_CAP_UNKNOWN);
    struct model_info held;
    EXPECT(model_meta_snapshot(&p, &held) == 1);
    EXPECT(held.context == 300000 && held.max_context == 872000);
    model_info_clear(&held);
    model_meta_release(&p);
}

/* Refreshing an unchanged model retries a failed probe once the finished job is reaped, instead
 * of mistaking the completed job for a live warm-up. */
static void test_refresh_retries_after_failed_probe(void)
{
    struct provider p = make_provider("llama.cpp", list_no_efforts);
    p.probe_model = counting_probe;
    probe_calls = 0;
    store_report(&p, "m", 0, PROVIDER_CAP_NO);
    model_meta_refresh(&p, "m");
    EXPECT(probe_calls == 1);

    /* The refused-connection probe fails within milliseconds; poll until a refresh reaps the
     * finished job and retries. The bound only matters on a pathologically slow machine. */
    const struct timespec poll_interval = {.tv_nsec = 1000000};
    for (int i = 0; i < 5000 && probe_calls < 2; i++) {
        model_meta_refresh(&p, "m");
        nanosleep(&poll_interval, NULL);
    }
    EXPECT(probe_calls == 2);
    model_meta_wait(&p);
    model_meta_release(&p);
}

/* Storing while a probe is in flight and the report is still empty must not merge from the
 * zeroed report, which would turn unknown pricing into a known $0. */
static void test_store_during_probe_keeps_costs_unknown(void)
{
    struct provider p = make_provider("llama.cpp", list_no_efforts);
    p.probe_model = counting_probe;
    probe_calls = 0;
    model_meta_refresh(&p, "m");

    store_report(&p, "m", 4096, PROVIDER_CAP_UNKNOWN);
    struct catalog_entry rates;
    EXPECT(model_meta_rates(&p, "m", &rates) == 0);
    EXPECT(rates.cost_input < 0 && rates.cost_output < 0);

    model_meta_wait(&p);
    model_meta_release(&p);
}

/* A list-derived report without a context window must not suppress the probe that learns it, and
 * must survive the probe failing. */
static void test_incomplete_report_still_probes(void)
{
    unsetenv("HAX_IMAGE_INPUT");
    struct provider p = make_provider("llama.cpp", list_no_efforts);
    p.probe_model = counting_probe;
    probe_calls = 0;

    store_report(&p, "qwen3", 0, PROVIDER_CAP_NO);
    model_meta_refresh(&p, "qwen3");
    model_meta_wait(&p);
    EXPECT(probe_calls == 1);
    EXPECT(model_meta_image_input(&p, "qwen3") == 0);

    store_report(&p, "qwen3", 32768, PROVIDER_CAP_NO);
    model_meta_refresh(&p, "qwen3");
    model_meta_wait(&p);
    EXPECT(probe_calls == 1);
    model_meta_release(&p);
}

static void test_provider_without_levels_stays_without_them(void)
{
    static const char *const reported[] = {"low", "high", NULL};
    struct provider p = make_provider("llama.cpp", list_no_efforts);
    store_efforts(&p, "qwen3", reported);
    struct effort_set levels;
    model_meta_efforts(&p, "qwen3", &levels);
    EXPECT(levels.known && levels.count == 0);
    model_meta_release(&p);
}

/* Exercise the metadata lifecycle through every provider destroy callback. */
static void test_release_is_honored_by_every_provider(void)
{
    size_t def_count = 0;
    const struct provider_def *const *defs = provider_all(&def_count);
    EXPECT(def_count > 0);
    int providers_created = 0;
    for (size_t i = 0; i <= def_count; i++) {
        const struct provider_def *def = (i < def_count) ? defs[i] : provider_find("mock");
        EXPECT(def != NULL);
        struct provider *provider = def ? provider_construct(def) : NULL;
        if (!provider)
            continue;
        providers_created++;
        model_meta_refresh(provider, "some-model");
        struct model_info info;
        model_info_init(&info);
        info.id = xstrdup("some-model");
        info.context = 1;
        model_meta_store(provider, &info);
        model_info_clear(&info);
        provider->destroy(provider);
    }
    EXPECT(providers_created > 0);
}

int main(void)
{
    write_catalog_fixture();
    test_rates_resolution();
    test_context_resolution();
    test_image_input_resolution();
    test_falls_back_to_provider_levels();
    test_provider_report_narrows();
    test_provider_report_extends_levels();
    test_catalog_narrows_but_cannot_widen();
    test_empty_report_removes_levels();
    test_report_is_scoped_to_provider_and_model();
    test_snapshot_can_restore_report();
    test_id_only_report_is_ignored();
    test_same_model_store_merges();
    test_configured_context_beats_report();
    test_configured_rates_beat_report();
    test_configured_empty_tiers_beat_tiered_report();
    test_configured_efforts_beat_report();
    test_same_model_store_keeps_max_context();
    test_refresh_retries_after_failed_probe();
    test_store_during_probe_keeps_costs_unknown();
    test_incomplete_report_still_probes();
    test_provider_without_levels_stays_without_them();
    test_release_is_honored_by_every_provider();
    T_REPORT();
}
