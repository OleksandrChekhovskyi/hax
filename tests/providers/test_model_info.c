/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "effort.h"
#include "harness.h"
#include "provider.h"
#include "providers/anthropic.h"
#include "providers/codex.h"
#include "providers/openrouter.h"

/* The three backends that describe their own models (openrouter, codex,
 * anthropic) each translate a private catalog shape into struct model_info.
 * These fixtures are trimmed from real responses. The invariant every case
 * shares: a field the entry doesn't carry stays at its unknown sentinel, so
 * model_meta falls back to the catalog instead of resolving a made-up
 * answer. Resolution between the two tiers is tested in test_model_meta.c. */

static json_t *parse(const char *json)
{
    json_error_t err;
    json_t *j = json_loads(json, 0, &err);
    if (!j)
        fprintf(stderr, "fixture parse failed: %s\n", err.text);
    return j;
}

/* Run `fn` over a fixture into a freshly initialized model_info. */
/* NOLINTBEGIN(bugprone-macro-parentheses): `m` names a declared variable */
#define WITH_ENTRY(json, fn, m)                                                                    \
    struct model_info m;                                                                           \
    model_info_init(&m);                                                                           \
    json_t *m##_j = parse(json);                                                                   \
    if (m##_j)                                                                                     \
        fn(m##_j, &m);
/* NOLINTEND(bugprone-macro-parentheses) */

/* ---------------- openrouter ---------------- */

static void test_openrouter_full(void)
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
    /* Per-token strings scale to the per-1M-token unit used everywhere else. */
    EXPECT(m.cost_input == 10.0);
    EXPECT(m.cost_output == 50.0);
    EXPECT(m.cost_cache_read == 1.0);
    /* Only the blurb's lead line — the rest is markdown paragraphs. */
    EXPECT_STR_EQ(m.description, "Fast-mode variant of [Opus 5](/x).");
    json_decref(m_j);
    free(m.description);
}

/* Trimmed from anthropic/claude-sonnet-4.5: the write rates, and the
 * `overrides` list OpenRouter uses for long-context tiers. */
static void test_openrouter_write_rates_and_tiers(void)
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
    /* Read, not assumed — the 2x-input fallback only happens to agree. */
    EXPECT(m.cost_cache_write_1h == 6.0);
    EXPECT(m.n_tiers == 1);
    /* Carried over unchanged: both thresholds are exclusive, so a prompt
     * of exactly 200k still bills at the base rates — the same answer the
     * catalog tier gives for this model. */
    EXPECT(m.tiers[0].context_threshold == 200000);
    EXPECT(m.tiers[0].cost_input == 6.0);
    EXPECT(m.tiers[0].cost_output == 22.5);
    EXPECT(m.tiers[0].cost_cache_read == 0.6);
    EXPECT(m.tiers[0].cost_cache_write == 7.5);
    /* A field the override omits stays unknown, which catalog_price reads
     * as "fall back to the base rate" rather than as free. */
    EXPECT(m.tiers[0].cost_cache_write_1h < 0);
    json_decref(m_j);
}

static void test_openrouter_flat_pricing_declares_no_tiers(void)
{
    /* Most of the catalog prices flat. No overrides must mean no tiers —
     * not a zero-threshold tier that would capture every request. */
    WITH_ENTRY("{\"pricing\":{\"prompt\":\"0.000001\",\"completion\":\"0.000006\"}}",
               openrouter_parse_model, m);
    EXPECT(m.n_tiers == 0);
    EXPECT(m.cost_cache_write < 0);
    EXPECT(m.cost_cache_write_1h < 0);
    json_decref(m_j);
}

static void test_openrouter_free_vs_variable(void)
{
    /* "0" is a genuinely free model and must survive as zero; "-1" marks a
     * variable price (the auto-routers) and must read as unknown, or the
     * picker would advertise a router as free. */
    WITH_ENTRY("{\"pricing\":{\"prompt\":\"0\",\"completion\":\"0\"}}", openrouter_parse_model, f);
    EXPECT(f.cost_input == 0.0);
    EXPECT(f.cost_output == 0.0);
    /* Roughly half the paid catalog quotes no cache rate at all; that must
     * stay unknown rather than collapse to "cached is free". */
    EXPECT(f.cost_cache_read < 0);
    json_decref(f_j);

    WITH_ENTRY("{\"pricing\":{\"prompt\":\"-1\",\"completion\":\"-1\"}}", openrouter_parse_model,
               v);
    EXPECT(v.cost_input < 0);
    EXPECT(v.cost_output < 0);
    json_decref(v_j);
}

static void test_openrouter_no_tools(void)
{
    /* A fifth of the catalog can't be given tools at all — an explicit "no",
     * distinct from a catalog that never mentions the field. */
    WITH_ENTRY("{\"supported_parameters\":[\"max_tokens\",\"stop\"],"
               "\"architecture\":{\"input_modalities\":[\"text\"]}}",
               openrouter_parse_model, m);
    EXPECT(m.tools == PROVIDER_CAP_NO);
    EXPECT(m.image_input == PROVIDER_CAP_NO);
    json_decref(m_j);
}

static void test_openrouter_bare(void)
{
    /* Nothing but an id: every field stays unknown rather than zero, so the
     * catalog tier still gets its chance. */
    WITH_ENTRY("{\"id\":\"vendor/model\"}", openrouter_parse_model, m);
    EXPECT(m.context == 0);
    EXPECT(m.image_input == PROVIDER_CAP_UNKNOWN);
    EXPECT(m.tools == PROVIDER_CAP_UNKNOWN);
    EXPECT(m.cost_input < 0);
    EXPECT(m.cost_cache_read < 0);
    EXPECT(m.description == NULL);
    json_decref(m_j);
}

/* Compare a parsed ladder against a NULL-terminated expectation, order
 * included — the picker paints these top to bottom, so order is part of the
 * answer, not an implementation detail. */
static int efforts_are(const struct effort_set *s, const char *const *want)
{
    size_t n = 0;
    while (want[n])
        n++;
    if (!s->known || s->count != n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (strcmp(s->values[i], want[i]) != 0)
            return 0;
    return 1;
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
    /* The response cap is its own limit, four times the fixed default the
     * request used to carry. */
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
    /* The same parser serves llama-server's Anthropic-compat endpoint, whose
     * entries carry none of this — everything must stay unknown. */
    WITH_ENTRY("{\"id\":\"local-model\",\"type\":\"model\"}", anthropic_parse_model, m);
    EXPECT(m.context == 0);
    EXPECT(m.image_input == PROVIDER_CAP_UNKNOWN);
    json_decref(m_j);
}

/* Anthropic states effort support as a flag plus one child per level. The
 * 4-5 generation says supported:false — a real answer ("this model takes a
 * thinking budget, not levels"), which must skip the /effort step rather
 * than fall through to a ladder it rejects. */
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
    /* Opus 4.6 supports max but not xhigh — the gap in the middle is why
     * the whole static ladder can't just be offered. Probed in ladder
     * order, so the menu reads low-to-high whatever order the JSON used. */
    WITH_ENTRY("{\"id\":\"claude-opus-4-6\",\"capabilities\":{\"effort\":{"
               "\"max\":{\"supported\":true},\"supported\":true,"
               "\"high\":{\"supported\":true},\"low\":{\"supported\":true},"
               "\"medium\":{\"supported\":true}}}}",
               anthropic_parse_model, m);
    static const char *const want[] = {"low", "medium", "high", "max", NULL};
    EXPECT(efforts_are(&m.efforts, want));
    json_decref(m_j);
}

static void test_anthropic_efforts_unknown_level(void)
{
    /* A level newer than this build still reaches the picker, appended
     * after the ones the ladder can order. */
    WITH_ENTRY("{\"id\":\"claude-opus-9\",\"capabilities\":{\"effort\":{\"supported\":true,"
               "\"low\":{\"supported\":true},\"ludicrous\":{\"supported\":true},"
               "\"high\":{\"supported\":false}}}}",
               anthropic_parse_model, m);
    EXPECT(m.efforts.known && m.efforts.count == 2);
    EXPECT(effort_set_has(&m.efforts, "low"));
    EXPECT(effort_set_has(&m.efforts, "ludicrous"));
    /* An explicit false is not support. */
    EXPECT(!effort_set_has(&m.efforts, "high"));
    json_decref(m_j);
}

/* ---------------- openrouter efforts ---------------- */

static void test_openrouter_efforts_verbatim(void)
{
    /* OpenRouter normalizes every upstream vocabulary into this one field,
     * so it needs no correction — but it does narrow hard, and it carries
     * levels hax's static openai ladder lacks ("max"). */
    struct effort_set s = {0};
    json_t *j = parse("{\"id\":\"x-ai/grok-4.5\",\"reasoning\":{\"mandatory\":true,"
                      "\"default_enabled\":true,\"supported_efforts\":[\"high\",\"medium\","
                      "\"low\"],\"default_effort\":\"high\"}}");
    openrouter_parse_efforts(j, &s);
    static const char *const want[] = {"high", "medium", "low", NULL};
    EXPECT(efforts_are(&s, want));
    json_decref(j);
}

static void test_openrouter_efforts_none(void)
{
    /* A third of the catalog can't reason at all. An entry that describes
     * its parameters but carries no reasoning block is a definite no, so
     * the effort step disappears instead of offering a dead ladder. */
    struct effort_set s = {0};
    json_t *j = parse("{\"id\":\"vendor/plain\",\"supported_parameters\":[\"tools\",\"stop\"]}");
    openrouter_parse_efforts(j, &s);
    EXPECT(s.known && s.count == 0);
    json_decref(j);

    /* Reasoning without levels (a toggle or token budget) is unknown, not
     * empty: the static ladder still applies. */
    struct effort_set b = {0};
    json_t *k = parse("{\"id\":\"moonshotai/kimi\",\"supported_parameters\":[\"reasoning\"],"
                      "\"reasoning\":{\"mandatory\":false,\"default_enabled\":true}}");
    openrouter_parse_efforts(k, &b);
    EXPECT(!b.known && b.count == 0);
    json_decref(k);

    /* The router entries describe no model yet, so they carry no reasoning
     * block — but they do take the parameter, and listing it has to keep
     * the ladder rather than hide /effort for them. */
    struct effort_set r = {0};
    json_t *a = parse("{\"id\":\"openrouter/auto\",\"supported_parameters\":"
                      "[\"tools\",\"include_reasoning\",\"reasoning\",\"reasoning_effort\"]}");
    openrouter_parse_efforts(a, &r);
    EXPECT(!r.known && r.count == 0);
    json_decref(a);

    /* An empty list is a stated no, distinct from the absent field above. */
    struct effort_set c = {0};
    json_t *q = parse("{\"id\":\"vendor/m\",\"reasoning\":{\"supported_efforts\":[]}}");
    openrouter_parse_efforts(q, &c);
    EXPECT(c.known && c.count == 0);
    json_decref(q);
}

/* ---------------- single-model probes ---------------- */

/* ?q= is a substring search, so the response routinely carries neighbours
 * of the model asked for — and sorts them first. Picking data[0] would
 * describe a different, usually pricier, model than the one in use. */
static void test_openrouter_meta_picks_the_exact_id(void)
{
    static const char BODY[] = "{\"data\":["
                               "{\"id\":\"openai/gpt-5.6-sol-pro\",\"context_length\":400000,"
                               " \"pricing\":{\"prompt\":\"0.000015\",\"completion\":\"0.00012\"}},"
                               "{\"id\":\"openai/gpt-5.6-sol\",\"context_length\":1050000,"
                               " \"top_provider\":{\"max_completion_tokens\":128000},"
                               " \"pricing\":{\"prompt\":\"0.000005\",\"completion\":\"0.00003\"},"
                               " \"reasoning\":{\"supported_efforts\":[\"high\",\"low\"]}}"
                               "]}";
    struct model_info m;
    model_info_init(&m);
    openrouter_parse_meta(BODY, "openai/gpt-5.6-sol", &m);
    EXPECT(m.context == 1050000);
    EXPECT(m.max_output == 128000);
    EXPECT(m.cost_input == 5.0);
    EXPECT(m.efforts.known && m.efforts.count == 2);
    model_info_clear(&m);

    /* A model the page doesn't carry leaves everything unknown rather than
     * adopting whatever came back. */
    struct model_info absent;
    model_info_init(&absent);
    openrouter_parse_meta(BODY, "vendor/other", &absent);
    EXPECT(absent.context == 0);
    EXPECT(!absent.efforts.known);
    model_info_clear(&absent);
}

static void test_openrouter_probe_url_escapes_the_id(void)
{
    /* Model ids carry '/' and ':' — legal in a path segment, but this one
     * travels as a query value, where they have to be encoded. */
    struct model_probe req;
    memset(&req, 0, sizeof(req));
    EXPECT(openrouter_probe_model(NULL, "meta-llama/llama-3.2-3b-instruct:free", &req) == 0);
    EXPECT_STR_EQ(req.url, "https://openrouter.ai/api/v1/models"
                           "?q=meta-llama%2Fllama-3.2-3b-instruct%3Afree");
    EXPECT(req.parse != NULL);
    model_probe_clear(&req);
    /* No model, nothing to fetch. */
    memset(&req, 0, sizeof(req));
    EXPECT(openrouter_probe_model(NULL, "", &req) == -1);
}

int main(void)
{
    test_openrouter_meta_picks_the_exact_id();
    test_openrouter_probe_url_escapes_the_id();
    test_openrouter_full();
    test_openrouter_write_rates_and_tiers();
    test_openrouter_flat_pricing_declares_no_tiers();
    test_openrouter_free_vs_variable();
    test_openrouter_no_tools();
    test_openrouter_bare();
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
    test_openrouter_efforts_verbatim();
    test_openrouter_efforts_none();
    T_REPORT();
}
