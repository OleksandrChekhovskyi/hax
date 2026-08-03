/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "model_meta.h"
#include "provider.h"
#include "util.h"
#include "providers/openai.h"

static void test_list_efforts_wiring(void)
{
    unsetenv("HAX_OPENAI_BASE_URL");

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
    test_list_efforts_wiring();
    test_cache_plan_follows_model_rates();
    T_REPORT();
}
