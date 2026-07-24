/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>

#include "harness.h"
#include "provider.h"
#include "providers/anthropic.h"
#include "providers/codex.h"
#include "providers/openrouter.h"

/* The three backends that describe their own models (openrouter, codex,
 * anthropic) each translate a private catalog shape into struct model_info.
 * These fixtures are trimmed from real responses. The invariant every case
 * shares: a field the entry doesn't carry stays at its unknown sentinel, so
 * the picker falls back to the catalog instead of showing a made-up answer.
 */

static json_t *parse(const char *json)
{
    json_error_t err;
    json_t *j = json_loads(json, 0, &err);
    if (!j)
        fprintf(stderr, "fixture parse failed: %s\n", err.text);
    return j;
}

/* Run `fn` over a fixture into a freshly initialized model_info. */
#define WITH_ENTRY(json, fn, m)                                                                    \
    struct model_info m;                                                                           \
    model_info_init(&m);                                                                           \
    json_t *m##_j = parse(json);                                                                   \
    if (m##_j)                                                                                     \
        fn(m##_j, &m);

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
    EXPECT_STR_EQ(m.desc, "Fast-mode variant of [Opus 5](/x).");
    json_decref(m_j);
    free(m.desc);
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
    EXPECT(m.desc == NULL);
    json_decref(m_j);
}

/* ---------------- codex ---------------- */

static void test_codex_serves_context_window(void)
{
    /* context_window is what requests actually get; max_context_window is the
     * ceiling. They differ, and the context-% display uses the former — so
     * the picker must too, or the two contradict each other. */
    WITH_ENTRY("{\"slug\":\"gpt-5.4\",\"context_window\":272000,"
               "\"max_context_window\":1000000,"
               "\"input_modalities\":[\"text\",\"image\"],"
               "\"description\":\"Strong model for everyday coding.\","
               "\"visibility\":\"list\"}",
               codex_parse_model, m);
    EXPECT(m.context == 272000);
    EXPECT(m.image_input == PROVIDER_CAP_YES);
    EXPECT_STR_EQ(m.desc, "Strong model for everyday coding.");
    EXPECT(!codex_model_hidden(m_j));
    json_decref(m_j);
    free(m.desc);
}

static void test_codex_context_fallback(void)
{
    WITH_ENTRY("{\"slug\":\"x\",\"max_context_window\":400000}", codex_parse_model, m);
    EXPECT(m.context == 400000);
    json_decref(m_j);
}

static void test_codex_hidden(void)
{
    /* The internal approval-review model: present in the catalog, never a
     * choice a user should be offered. */
    json_t *j = parse("{\"slug\":\"codex-auto-review\",\"visibility\":\"hide\"}");
    EXPECT(codex_model_hidden(j));
    json_decref(j);

    /* No visibility field at all is not hidden. */
    json_t *k = parse("{\"slug\":\"x\"}");
    EXPECT(!codex_model_hidden(k));
    json_decref(k);
}

/* ---------------- anthropic ---------------- */

static void test_anthropic_capabilities(void)
{
    WITH_ENTRY("{\"id\":\"claude-opus-5\",\"max_input_tokens\":1000000,"
               "\"max_tokens\":128000,"
               "\"capabilities\":{\"image_input\":{\"supported\":true}}}",
               anthropic_parse_model, m);
    EXPECT(m.context == 1000000);
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

int main(void)
{
    test_openrouter_full();
    test_openrouter_free_vs_variable();
    test_openrouter_no_tools();
    test_openrouter_bare();
    test_codex_serves_context_window();
    test_codex_context_fallback();
    test_codex_hidden();
    test_anthropic_capabilities();
    test_anthropic_capability_false();
    test_anthropic_compat_shape();
    T_REPORT();
}
