/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "effort.h"
#include "harness.h"
#include "provider.h"
#include "providers/anthropic_models.h"
#include "providers/codex.h"
#include "providers/openrouter.h"

/* Provider catalog fixtures translate private response shapes into model_info. Missing fields stay
 * unknown so model_meta can fall back to the shared catalog. */

static json_t *parse(const char *json)
{
    json_error_t err;
    json_t *j = json_loads(json, 0, &err);
    if (!j)
        fprintf(stderr, "fixture parse failed: %s\n", err.text);
    return j;
}

/* NOLINTBEGIN(bugprone-macro-parentheses): `m` names a declared variable */
#define WITH_ENTRY(json, fn, m)                                                                    \
    struct model_info m;                                                                           \
    model_info_init(&m);                                                                           \
    json_t *m##_j = parse(json);                                                                   \
    if (m##_j)                                                                                     \
        fn(m##_j, &m);
/* NOLINTEND(bugprone-macro-parentheses) */

/* Effort order controls picker order, so compare it as well as membership. */
static int efforts_are(const struct effort_set *efforts, const char *const *expected)
{
    size_t count = 0;
    while (expected[count])
        count++;
    if (!efforts->known || efforts->count != count)
        return 0;
    for (size_t i = 0; i < count; i++)
        if (strcmp(efforts->values[i], expected[i]) != 0)
            return 0;
    return 1;
}

/* ---------------- openrouter ---------------- */

static void test_openrouter_model_metadata(void)
{
    WITH_ENTRY("{\"id\":\"anthropic/claude-opus-5-fast\","
               "\"description\":\"Fast-mode variant of [Opus 5](/x).\\n\\nLearn more in docs.\","
               "\"context_length\":1000000,"
               "\"architecture\":{\"input_modalities\":[\"text\",\"image\",\"file\"]},"
               "\"pricing\":{\"prompt\":\"0.00001\",\"completion\":\"0.00005\","
               "\"input_cache_read\":\"0.000001\"},"
               "\"supported_parameters\":[\"tools\",\"reasoning\"]}",
               openrouter_parse_model, m);
    EXPECT(m.context == 1000000);
    EXPECT(m.image_input == PROVIDER_CAP_YES);
    EXPECT(m.tools == PROVIDER_CAP_YES);
    EXPECT(m.cost_input == 10.0);
    EXPECT(m.cost_output == 50.0);
    EXPECT(m.cost_cache_read == 1.0);
    EXPECT_STR_EQ(m.description, "Fast-mode variant of [Opus 5](/x).");
    json_decref(m_j);
    model_info_clear(&m);
}

static void test_openrouter_pricing_tiers(void)
{
    WITH_ENTRY("{\"id\":\"anthropic/claude-sonnet-4.5\","
               "\"pricing\":{\"prompt\":\"0.000003\",\"completion\":\"0.000015\","
               "\"input_cache_read\":\"0.0000003\",\"input_cache_write\":\"0.00000375\","
               "\"input_cache_write_1h\":\"0.000006\","
               "\"overrides\":[{\"min_prompt_tokens\":200000,\"prompt\":\"0.000006\","
               "\"completion\":\"0.0000225\",\"input_cache_read\":\"0.0000006\","
               "\"input_cache_write\":\"0.0000075\"}]}}",
               openrouter_parse_model, m);
    EXPECT(m.cost_cache_write == 3.75);
    EXPECT(m.cost_cache_write_1h == 6.0);
    EXPECT(m.n_tiers == 1);
    /* Both threshold representations are exclusive, so exactly 200k uses the base rates. */
    EXPECT(m.tiers[0].context_threshold == 200000);
    EXPECT(m.tiers[0].cost_input == 6.0);
    EXPECT(m.tiers[0].cost_output == 22.5);
    EXPECT(m.tiers[0].cost_cache_read == 0.6);
    EXPECT(m.tiers[0].cost_cache_write == 7.5);
    /* A field the override omits stays unknown, which catalog_price reads
     * as "fall back to the base rate" rather than as free. */
    EXPECT(m.tiers[0].cost_cache_write_1h < 0);
    json_decref(m_j);
    model_info_clear(&m);
}

static void test_openrouter_no_pricing_tiers(void)
{
    WITH_ENTRY("{\"pricing\":{\"prompt\":\"0.000001\",\"completion\":\"0.000006\"}}",
               openrouter_parse_model, m);
    EXPECT(m.n_tiers == 0);
    EXPECT(m.cost_cache_write < 0);
    EXPECT(m.cost_cache_write_1h < 0);
    json_decref(m_j);
    model_info_clear(&m);
}

static void test_openrouter_price_validation(void)
{
    WITH_ENTRY("{\"pricing\":{\"prompt\":\"0\",\"completion\":\"0\"}}", openrouter_parse_model,
               free_model);
    EXPECT(free_model.cost_input == 0.0);
    EXPECT(free_model.cost_output == 0.0);
    EXPECT(free_model.cost_cache_read < 0);
    json_decref(free_model_j);
    model_info_clear(&free_model);

    WITH_ENTRY("{\"pricing\":{\"prompt\":\"-1\",\"completion\":\"1junk\"}}", openrouter_parse_model,
               unknown_price);
    EXPECT(unknown_price.cost_input < 0);
    EXPECT(unknown_price.cost_output < 0);
    json_decref(unknown_price_j);
    model_info_clear(&unknown_price);
}

static void test_openrouter_capabilities(void)
{
    WITH_ENTRY("{\"supported_parameters\":[\"max_tokens\",\"stop\"],"
               "\"architecture\":{\"input_modalities\":[\"text\"]}}",
               openrouter_parse_model, m);
    EXPECT(m.tools == PROVIDER_CAP_NO);
    EXPECT(m.image_input == PROVIDER_CAP_NO);
    json_decref(m_j);
    model_info_clear(&m);
}

static void test_openrouter_missing_metadata(void)
{
    WITH_ENTRY("{\"id\":\"vendor/model\"}", openrouter_parse_model, m);
    EXPECT(m.context == 0);
    EXPECT(m.image_input == PROVIDER_CAP_UNKNOWN);
    EXPECT(m.tools == PROVIDER_CAP_UNKNOWN);
    EXPECT(m.cost_input < 0);
    EXPECT(m.cost_cache_read < 0);
    EXPECT(m.description == NULL);
    json_decref(m_j);
    model_info_clear(&m);
}

static void test_openrouter_effort_levels(void)
{
    struct effort_set efforts = {0};
    json_t *entry = parse("{\"id\":\"x-ai/grok-4.5\",\"reasoning\":{\"mandatory\":true,"
                          "\"default_enabled\":true,\"supported_efforts\":[\"high\",\"medium\","
                          "\"low\"],\"default_effort\":\"high\"}}");
    openrouter_parse_efforts(entry, &efforts);
    static const char *const expected[] = {"high", "medium", "low", NULL};
    EXPECT(efforts_are(&efforts, expected));
    json_decref(entry);
}

static void test_openrouter_effort_metadata_states(void)
{
    struct effort_set unsupported = {0};
    json_t *plain_model =
        parse("{\"id\":\"vendor/plain\",\"supported_parameters\":[\"tools\",\"stop\"]}");
    openrouter_parse_efforts(plain_model, &unsupported);
    EXPECT(unsupported.known && unsupported.count == 0);
    json_decref(plain_model);

    struct effort_set uncategorized = {0};
    json_t *reasoning_toggle =
        parse("{\"id\":\"moonshotai/kimi\",\"supported_parameters\":[\"reasoning\"],"
              "\"reasoning\":{\"mandatory\":false,\"default_enabled\":true}}");
    openrouter_parse_efforts(reasoning_toggle, &uncategorized);
    EXPECT(!uncategorized.known && uncategorized.count == 0);
    json_decref(reasoning_toggle);

    struct effort_set routed = {0};
    json_t *router = parse("{\"id\":\"openrouter/auto\",\"supported_parameters\":"
                           "[\"tools\",\"include_reasoning\",\"reasoning\","
                           "\"reasoning_effort\"]}");
    openrouter_parse_efforts(router, &routed);
    EXPECT(!routed.known && routed.count == 0);
    json_decref(router);

    struct effort_set empty = {0};
    json_t *empty_levels = parse("{\"id\":\"vendor/m\",\"reasoning\":{\"supported_efforts\":[]}}");
    openrouter_parse_efforts(empty_levels, &empty);
    EXPECT(empty.known && empty.count == 0);
    json_decref(empty_levels);
}

/* ?q= is a substring search, so match the requested model's exact id. */
static void test_openrouter_probe_exact_model(void)
{
    static const char BODY[] = "{\"data\":["
                               "{\"id\":\"openai/gpt-5.6-sol-pro\",\"context_length\":400000,"
                               " \"pricing\":{\"prompt\":\"0.000015\",\"completion\":\"0.00012\"}},"
                               "{\"id\":\"openai/gpt-5.6-sol\",\"context_length\":1050000,"
                               " \"top_provider\":{\"max_completion_tokens\":128000},"
                               " \"pricing\":{\"prompt\":\"0.000005\",\"completion\":\"0.00003\"},"
                               " \"reasoning\":{\"supported_efforts\":[\"high\",\"low\"]}}"
                               "]}";
    struct model_info model;
    model_info_init(&model);
    openrouter_parse_model_probe_response(BODY, "openai/gpt-5.6-sol", &model);
    EXPECT(model.context == 1050000);
    EXPECT(model.max_output == 128000);
    EXPECT(model.cost_input == 5.0);
    EXPECT(model.efforts.known && model.efforts.count == 2);
    model_info_clear(&model);

    struct model_info absent;
    model_info_init(&absent);
    openrouter_parse_model_probe_response(BODY, "vendor/other", &absent);
    EXPECT(absent.context == 0);
    EXPECT(!absent.efforts.known);
    model_info_clear(&absent);
}

static void test_openrouter_probe_url_encoding(void)
{
    struct model_probe probe = {0};
    EXPECT(openrouter_probe_model(NULL, "meta-llama/llama-3.2-3b-instruct:free", &probe) == 0);
    EXPECT_STR_EQ(probe.url, "https://openrouter.ai/api/v1/models"
                             "?q=meta-llama%2Fllama-3.2-3b-instruct%3Afree");
    EXPECT(probe.parse != NULL);
    model_probe_clear(&probe);

    EXPECT(openrouter_probe_model(NULL, "", &probe) == -1);
}

/* ---------------- codex ---------------- */

static void test_codex_model_capabilities(void)
{
    WITH_ENTRY("{\"slug\":\"gpt-5.4\",\"context_window\":272000,"
               "\"max_context_window\":1000000,"
               "\"input_modalities\":[\"text\",\"image\"],"
               "\"description\":\"Strong model for everyday coding.\","
               "\"visibility\":\"list\"}",
               codex_parse_model, model);
    EXPECT(model.context == 272000);
    EXPECT(model.max_context == 1000000);
    EXPECT(model.image_input == PROVIDER_CAP_YES);
    EXPECT_STR_EQ(model.description, "Strong model for everyday coding.");
    EXPECT(!codex_model_is_hidden(model_j));
    json_decref(model_j);
    free(model.description);
}

static void test_codex_context_fallback(void)
{
    WITH_ENTRY("{\"slug\":\"x\",\"max_context_window\":400000}", codex_parse_model, model);
    EXPECT(model.context == 400000);
    EXPECT(model.max_context == 400000);
    json_decref(model_j);
}

static void test_codex_hidden_models(void)
{
    json_t *hidden = parse("{\"slug\":\"codex-auto-review\",\"visibility\":\"hide\"}");
    EXPECT(codex_model_is_hidden(hidden));
    json_decref(hidden);

    json_t *visible = parse("{\"slug\":\"x\"}");
    EXPECT(!codex_model_is_hidden(visible));
    json_decref(visible);
}

static void test_codex_effort_ladder_normalized(void)
{
    struct effort_set efforts = {0};
    json_t *entry = parse("{\"slug\":\"gpt-5.6-sol\",\"supported_reasoning_levels\":["
                          "{\"effort\":\"low\",\"description\":\"Fast responses\"},"
                          "{\"effort\":\"medium\",\"description\":\"Balances speed\"},"
                          "{\"effort\":\"high\",\"description\":\"Greater depth\"},"
                          "{\"effort\":\"xhigh\",\"description\":\"Extra high\"},"
                          "{\"effort\":\"max\",\"description\":\"Maximum depth\"},"
                          "{\"effort\":\"ultra\",\"description\":\"With delegation\"}]}");
    codex_parse_model_efforts(entry, &efforts);
    static const char *const expected[] = {"none", "low", "medium", "high", "xhigh", "max", NULL};
    EXPECT(efforts_are(&efforts, expected));
    EXPECT(!effort_set_has(&efforts, "ultra"));
    EXPECT(!effort_set_has(&efforts, "minimal"));
    json_decref(entry);
}

static void test_codex_bare_effort_levels(void)
{
    struct effort_set efforts = {0};
    json_t *entry = parse("{\"supported_reasoning_levels\":[\"low\",\"medium\",\"high\"]}");
    codex_parse_model_efforts(entry, &efforts);
    static const char *const expected[] = {"none", "low", "medium", "high", NULL};
    EXPECT(efforts_are(&efforts, expected));
    json_decref(entry);
}

static void test_codex_effort_metadata_states(void)
{
    struct effort_set absent = {0};
    json_t *without_levels = parse("{\"slug\":\"x\",\"context_window\":272000}");
    codex_parse_model_efforts(without_levels, &absent);
    EXPECT(!absent.known && absent.count == 0);
    json_decref(without_levels);

    struct effort_set empty = {0};
    json_t *with_empty_levels = parse("{\"slug\":\"x\",\"supported_reasoning_levels\":[]}");
    codex_parse_model_efforts(with_empty_levels, &empty);
    EXPECT(empty.known && empty.count == 0);
    json_decref(with_empty_levels);
}

/* ---------------- anthropic ---------------- */

static void test_anthropic_capabilities(void)
{
    WITH_ENTRY("{\"id\":\"claude-opus-5\",\"max_input_tokens\":1000000,"
               "\"max_tokens\":128000,"
               "\"capabilities\":{\"image_input\":{\"supported\":true}}}",
               anthropic_parse_model, m);
    EXPECT(m.context == 1000000);
    EXPECT(m.max_output == 128000);
    EXPECT(m.image_input == PROVIDER_CAP_YES);
    json_decref(m_j);
}

static void test_anthropic_capability_false(void)
{
    WITH_ENTRY("{\"id\":\"x\",\"capabilities\":{\"image_input\":{\"supported\":false}}}",
               anthropic_parse_model, m);
    EXPECT(m.image_input == PROVIDER_CAP_NO);
    json_decref(m_j);
}

static void test_anthropic_compat_shape(void)
{
    WITH_ENTRY("{\"id\":\"local-model\",\"type\":\"model\"}", anthropic_parse_model, m);
    EXPECT(m.context == 0);
    EXPECT(m.image_input == PROVIDER_CAP_UNKNOWN);
    json_decref(m_j);
}

static void test_anthropic_efforts_unsupported(void)
{
    WITH_ENTRY("{\"id\":\"claude-haiku-4-5\",\"capabilities\":{\"effort\":{\"supported\":false},"
               "\"thinking\":{\"supported\":true,\"types\":{\"enabled\":{\"supported\":true}}}}}",
               anthropic_parse_model, m);
    EXPECT(m.efforts.known && m.efforts.count == 0);
    json_decref(m_j);
}

static void test_anthropic_efforts_partial_ladder(void)
{
    WITH_ENTRY("{\"id\":\"claude-opus-4-6\",\"capabilities\":{\"effort\":{"
               "\"max\":{\"supported\":true},\"supported\":true,"
               "\"high\":{\"supported\":true},\"low\":{\"supported\":true},"
               "\"medium\":{\"supported\":true}}}}",
               anthropic_parse_model, m);
    static const char *const expected[] = {"low", "medium", "high", "max", NULL};
    EXPECT(efforts_are(&m.efforts, expected));
    json_decref(m_j);
}

static void test_anthropic_efforts_unknown_level(void)
{
    WITH_ENTRY("{\"id\":\"claude-opus-9\",\"capabilities\":{\"effort\":{\"supported\":true,"
               "\"low\":{\"supported\":true},\"ludicrous\":{\"supported\":true},"
               "\"high\":{\"supported\":false}}}}",
               anthropic_parse_model, m);
    EXPECT(m.efforts.known && m.efforts.count == 2);
    EXPECT(effort_set_has(&m.efforts, "low"));
    EXPECT(effort_set_has(&m.efforts, "ludicrous"));
    EXPECT(!effort_set_has(&m.efforts, "high"));
    json_decref(m_j);
}

int main(void)
{
    test_openrouter_model_metadata();
    test_openrouter_pricing_tiers();
    test_openrouter_no_pricing_tiers();
    test_openrouter_price_validation();
    test_openrouter_capabilities();
    test_openrouter_missing_metadata();
    test_openrouter_effort_levels();
    test_openrouter_effort_metadata_states();
    test_openrouter_probe_exact_model();
    test_openrouter_probe_url_encoding();
    test_codex_model_capabilities();
    test_codex_context_fallback();
    test_codex_hidden_models();
    test_codex_effort_ladder_normalized();
    test_codex_bare_effort_levels();
    test_codex_effort_metadata_states();
    test_anthropic_capabilities();
    test_anthropic_capability_false();
    test_anthropic_compat_shape();
    test_anthropic_efforts_unsupported();
    test_anthropic_efforts_partial_ladder();
    test_anthropic_efforts_unknown_level();
    T_REPORT();
}
