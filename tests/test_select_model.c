/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "catalog.h"
#include "harness.h"
#include "provider.h"
#include "select.h"

static struct catalog_entry unknown_catalog_entry(void)
{
    struct catalog_entry entry;
    catalog_entry_init(&entry);
    return entry;
}

static void test_reported_full(void)
{
    struct model_info model;
    model_info_init(&model);
    model.id = "vendor/model";
    model.context = 1000000;
    model.image_input = PROVIDER_CAP_YES;
    model.tools = PROVIDER_CAP_YES;
    model.cost_input = 10;
    model.cost_cache_read = 1;
    model.cost_output = 50;
    model.description = "Fast-mode variant.";

    char *description = model_desc_line(&model, NULL, NULL);
    EXPECT_STR_EQ(description,
                  "1M context · $10 in / $1 cached / $50 out per Mtok\nFast-mode variant.");
    free(description);
}

static void test_catalog_fills_gaps(void)
{
    struct model_info model;
    model_info_init(&model);
    model.context = 272000;

    struct catalog_entry catalog = unknown_catalog_entry();
    catalog.context_window = 400000;
    catalog.image_input = CATALOG_SUPPORT_YES;
    catalog.cost_input = 1.25;
    catalog.cost_cache_read = 0.125;
    catalog.cost_output = 10;

    char *description = model_desc_line(&model, NULL, &catalog);
    EXPECT_STR_EQ(description, "272k context · $1.25 in / $0.125 cached / $10 out per Mtok");
    free(description);
}

static void test_unknown_fields_omitted(void)
{
    struct model_info model;
    model_info_init(&model);
    model.id = "some-model";

    EXPECT(model_desc_line(&model, NULL, NULL) == NULL);

    struct catalog_entry catalog = unknown_catalog_entry();
    EXPECT(model_desc_line(&model, NULL, &catalog) == NULL);
}

static void test_override_ceiling_shown(void)
{
    /* codex serves a default window below the model's sanctioned override ceiling. */
    struct model_info model;
    model_info_init(&model);
    model.context = 272000;
    model.max_context = 872000;

    char *description = model_desc_line(&model, NULL, NULL);
    EXPECT_STR_EQ(description, "272k context (up to 872k)");
    free(description);

    /* A configured override raised to the ceiling leaves nothing to advertise. */
    struct catalog_entry configured = unknown_catalog_entry();
    configured.context_window = 872000;
    description = model_desc_line(&model, &configured, NULL);
    EXPECT_STR_EQ(description, "872k context");
    free(description);
}

static void test_image_input_only_when_absent(void)
{
    /* Multimodal input is the norm, so only its absence is worth saying. */
    struct model_info model;
    model_info_init(&model);
    model.context = 32000;
    model.image_input = PROVIDER_CAP_NO;

    char *description = model_desc_line(&model, NULL, NULL);
    EXPECT_STR_EQ(description, "32k context · no images");
    free(description);

    model_info_init(&model);
    model.context = 32000;
    model.image_input = PROVIDER_CAP_YES;
    description = model_desc_line(&model, NULL, NULL);
    EXPECT_STR_EQ(description, "32k context");
    free(description);
}

static void test_tools_stay_off_the_gutter(void)
{
    /* Tool support dims the row and names itself in the row's detail; it
     * must not also consume a gutter segment, or every unusable model says
     * the same thing twice. */
    struct model_info model;
    model_info_init(&model);
    model.context = 32000;
    model.tools = PROVIDER_CAP_NO;

    char *description = model_desc_line(&model, NULL, NULL);
    EXPECT_STR_EQ(description, "32k context");
    free(description);

    model_info_init(&model);
    model.tools = PROVIDER_CAP_NO;
    EXPECT(model_desc_line(&model, NULL, NULL) == NULL);
}

static void test_power_of_two_window(void)
{
    /* Real windows are usually powers of two (1048576, 262144). Users know
     * those as "1M" and "262k", not "1.05M" / "1048k". */
    struct model_info model;
    model_info_init(&model);
    model.context = 1048576;

    char *description = model_desc_line(&model, NULL, NULL);
    EXPECT_STR_EQ(description, "1M context");
    free(description);

    model_info_init(&model);
    model.context = 1500000;
    description = model_desc_line(&model, NULL, NULL);
    EXPECT_STR_EQ(description, "1.5M context");
    free(description);
}

static void test_free_model(void)
{
    /* Zero rates are a real answer (free tier), distinct from unknown. */
    struct model_info model;
    model_info_init(&model);
    model.cost_input = 0;
    model.cost_output = 0;

    char *description = model_desc_line(&model, NULL, NULL);
    EXPECT_STR_EQ(description, "free");
    free(description);
}

static void test_description_only(void)
{
    struct model_info model;
    model_info_init(&model);
    model.description = "Latest frontier agentic coding model.";

    char *description = model_desc_line(&model, NULL, NULL);
    EXPECT_STR_EQ(description, "Latest frontier agentic coding model.");
    free(description);
}

int main(void)
{
    test_reported_full();
    test_catalog_fills_gaps();
    test_unknown_fields_omitted();
    test_override_ceiling_shown();
    test_image_input_only_when_absent();
    test_tools_stay_off_the_gutter();
    test_power_of_two_window();
    test_free_model();
    test_description_only();
    T_REPORT();
}
