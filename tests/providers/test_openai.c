/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "harness.h"
#include "model_meta.h"
#include "provider.h"
#include "util.h"
#include "providers/openai.h"

/* The compat env aliases land in providers.openai-compatible.*; the first-party provider reads
 * only its own providers.openai block, so a key or name meant for a compatible endpoint cannot
 * alter it. */
static void test_first_party_ignores_compat_config(void)
{
    setenv("HAX_OPENAI_DISPLAY_NAME", "Renamed", 1);
    setenv("HAX_OPENAI_API_KEY", "sk-compat", 1);
    unsetenv("OPENAI_API_KEY");

    struct provider *provider = PROVIDER_OPENAI.new("openai");
    EXPECT(provider != NULL);
    if (provider) {
        EXPECT_STR_EQ(provider->name, "openai");
        provider->destroy(provider);
    }

    struct provider_availability availability = {0};
    PROVIDER_OPENAI.prepare_availability("openai", &availability);
    EXPECT(!availability.available);
    EXPECT_STR_EQ(availability.reason, "OPENAI_API_KEY not set");
    provider_availability_clear(&availability);

    /* An inline key in the provider's own block makes it available, matching construction. */
    config_set_override("providers.openai.api_key", "sk-inline");
    PROVIDER_OPENAI.prepare_availability("openai", &availability);
    EXPECT(availability.available);
    provider_availability_clear(&availability);
    config_set_override("providers.openai.api_key", NULL);

    /* A pinned base_url and a config-provider-only field warn instead of silently vanishing.
     * The field check lints the config file, so these arrive through the file tier. */
    EXPECT(config_load("{\"providers\": {\"openai\": {\"base_url\": \"http://127.0.0.1:1\","
                       " \"sort_models\": \"on\"}}}") == 0);
    unsigned long diagnostics_before = hax_diag_sequence();
    provider = PROVIDER_OPENAI.new("openai");
    EXPECT(hax_diag_sequence() == diagnostics_before + 2);
    EXPECT(provider != NULL);
    if (provider)
        provider->destroy(provider);
    EXPECT(config_load(NULL) == 0);

    /* The provider's own block renames the banner; the provenance id stays stable. */
    config_set_override("providers.openai.display_name", "Work OpenAI");
    provider = PROVIDER_OPENAI.new("openai");
    EXPECT(provider != NULL);
    if (provider) {
        EXPECT_STR_EQ(provider->name, "Work OpenAI");
        EXPECT_STR_EQ(provider->id, "openai");
        EXPECT_STR_EQ(provider_stable_id(provider), "openai");
        provider->destroy(provider);
    }
    config_set_override("providers.openai.display_name", NULL);

    unsetenv("HAX_OPENAI_DISPLAY_NAME");
    unsetenv("HAX_OPENAI_API_KEY");
}

static void test_list_efforts_wiring(void)
{
    struct openai_preset with_efforts = {
        .display_name = "with-efforts",
        .default_base_url = "http://example.invalid/v1",
        .efforts = OPENAI_EFFORT_LADDER,
        .n_efforts = OPENAI_EFFORT_LADDER_N,
    };
    struct provider *provider = openai_provider_new_preset(&with_efforts);
    EXPECT(provider != NULL);
    if (provider) {
        EXPECT(provider->list_models != NULL);
        EXPECT(provider->list_efforts != NULL);
        const char *const *efforts = NULL;
        size_t n_efforts = provider->list_efforts(provider, &efforts);
        EXPECT(n_efforts == OPENAI_EFFORT_LADDER_N);
        EXPECT(efforts != NULL && strcmp(efforts[0], "none") == 0);
        EXPECT(strcmp(efforts[n_efforts - 1], "max") == 0);
        provider->destroy(provider);
    }

    struct openai_preset without_efforts = {
        .display_name = "without-efforts",
        .default_base_url = "http://example.invalid/v1",
    };
    provider = openai_provider_new_preset(&without_efforts);
    EXPECT(provider != NULL);
    if (provider) {
        const char *const *efforts = NULL;
        EXPECT(provider->list_efforts(provider, &efforts) == 0);
        provider->destroy(provider);
    }
}

static void test_wire_parse(void)
{
    EXPECT(openai_wire_parse("responses", OPENAI_WIRE_CHAT) == OPENAI_WIRE_RESPONSES);
    EXPECT(openai_wire_parse("chat", OPENAI_WIRE_RESPONSES) == OPENAI_WIRE_CHAT);
    /* The config-provider dialect names must resolve to the same two protocols. */
    EXPECT(openai_wire_parse("openai-completions", OPENAI_WIRE_RESPONSES) == OPENAI_WIRE_CHAT);
    EXPECT(openai_wire_parse("openai-responses", OPENAI_WIRE_CHAT) == OPENAI_WIRE_RESPONSES);
    EXPECT(openai_wire_parse(NULL, OPENAI_WIRE_RESPONSES) == OPENAI_WIRE_RESPONSES);
    EXPECT(openai_wire_parse("", OPENAI_WIRE_RESPONSES) == OPENAI_WIRE_RESPONSES);
    EXPECT(openai_wire_parse("grpc", OPENAI_WIRE_CHAT) == OPENAI_WIRE_CHAT);
}

static void store_model(struct provider *provider, struct model_info *model)
{
    model_meta_store(provider, model);
    model_info_clear(model);
}

static void test_cache_plan_follows_model_rates(void)
{
    struct provider provider = {0};
    struct model_info model;

    model_info_init(&model);
    model.id = xstrdup("anthropic-ish");
    model.cost_input = 3;
    model.cost_output = 15;
    model.cost_cache_write = 3.75;
    model.cost_cache_write_1h = 6;
    store_model(&provider, &model);

    struct openai_cache_plan plan =
        openai_plan_cache(&provider, "anthropic-ish", OPENAI_CACHE_AUTO, "1h");
    EXPECT(plan.send_breakpoints == 1);
    EXPECT(plan.writes_bill_1h == 1);

    plan = openai_plan_cache(&provider, "anthropic-ish", OPENAI_CACHE_AUTO, "5m");
    EXPECT(plan.send_breakpoints == 1);
    EXPECT(plan.writes_bill_1h == 0);

    plan = openai_plan_cache(&provider, "anthropic-ish", OPENAI_CACHE_OFF, "1h");
    EXPECT(plan.send_breakpoints == 0);
    EXPECT(plan.writes_bill_1h == 0);

    /* A write rate without a quoted 1h rate must not use the 1h billing fallback. */
    model_info_init(&model);
    model.id = xstrdup("openai-ish");
    model.cost_input = 1;
    model.cost_output = 6;
    model.cost_cache_write = 1.25;
    store_model(&provider, &model);

    plan = openai_plan_cache(&provider, "openai-ish", OPENAI_CACHE_AUTO, "1h");
    EXPECT(plan.send_breakpoints == 1);
    EXPECT(plan.writes_bill_1h == 0);

    /* A cache-write surcharge does not replace input processing, so AUTO declines it. */
    model_info_init(&model);
    model.id = xstrdup("gemini-ish");
    model.cost_input = 2;
    model.cost_output = 12;
    model.cost_cache_read = 0.2;
    model.cost_cache_write = 0.375;
    store_model(&provider, &model);

    plan = openai_plan_cache(&provider, "gemini-ish", OPENAI_CACHE_AUTO, "1h");
    EXPECT(plan.send_breakpoints == 0);
    EXPECT(plan.writes_bill_1h == 0);

    plan = openai_plan_cache(&provider, "gemini-ish", OPENAI_CACHE_ON, "1h");
    EXPECT(plan.send_breakpoints == 1);
    EXPECT(plan.writes_bill_1h == 0);

    plan = openai_plan_cache(&provider, "unknown", OPENAI_CACHE_AUTO, "1h");
    EXPECT(plan.send_breakpoints == 1);
    EXPECT(plan.writes_bill_1h == 0);

    model_meta_release(&provider);
}

int main(void)
{
    test_first_party_ignores_compat_config();
    test_list_efforts_wiring();
    test_wire_parse();
    test_cache_plan_follows_model_rates();
    T_REPORT();
}
