/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "harness.h"
#include "catalog.h"
#include "provider.h"
#include "select.h"

/* model_desc_line composes the /model picker's gutter from two sources:
 * what the backend reported about a model, and the catalog entry for it.
 * The rules under test are the layering (reported wins field by field,
 * catalog fills gaps) and the omission policy (an unknown field produces no
 * segment at all — never a zero). */

/* A catalog entry with everything unknown, matching what catalog_lookup
 * leaves behind on a miss. */
static struct catalog_entry cat_unknown(void)
{
    struct catalog_entry e = {0};
    e.cost_input = e.cost_output = e.cost_cache_read = e.cost_cache_write = -1;
    e.image_input = -1;
    return e;
}

static void test_reported_full(void)
{
    struct model_info m;
    model_info_init(&m);
    m.id = "vendor/model";
    m.context = 1000000;
    m.image_input = PROVIDER_CAP_YES;
    m.tools = PROVIDER_CAP_YES;
    m.cost_input = 10;
    m.cost_cache_read = 1;
    m.cost_output = 50;
    m.desc = "Fast-mode variant.";

    /* Multimodal + tools are the norm and stay unsaid; prose gets its own
     * footer line so it can't run into the structured fields. */
    char *s = model_desc_line(&m, NULL);
    EXPECT_STR_EQ(s, "1M context · $10 in / $1 cached / $50 out per Mtok\nFast-mode variant.");
    free(s);
}

static void test_catalog_fills_gaps(void)
{
    /* Backend reported only the window; the rest comes from the catalog. */
    struct model_info m;
    model_info_init(&m);
    m.context = 272000;

    struct catalog_entry c = cat_unknown();
    c.context = 400000; /* loses to the reported value */
    c.image_input = 1;
    c.cost_input = 1.25;
    c.cost_cache_read = 0.125;
    c.cost_output = 10;

    char *s = model_desc_line(&m, &c);
    EXPECT_STR_EQ(s, "272k context · $1.25 in / $0.125 cached / $10 out per Mtok");
    free(s);
}

static void test_unknown_fields_omitted(void)
{
    /* A bare-ids backend with no catalog presence earns no gutter at all,
     * rather than a row of zeros. */
    struct model_info m;
    model_info_init(&m);
    m.id = "some-model";

    EXPECT(model_desc_line(&m, NULL) == NULL);

    struct catalog_entry c = cat_unknown();
    EXPECT(model_desc_line(&m, &c) == NULL);
}

static void test_image_input_only_when_absent(void)
{
    /* Multimodal input is the norm, so only its absence is worth saying. */
    struct model_info m;
    model_info_init(&m);
    m.context = 32000;
    m.image_input = PROVIDER_CAP_NO;

    char *s = model_desc_line(&m, NULL);
    EXPECT_STR_EQ(s, "32k context · no images");
    free(s);

    model_info_init(&m);
    m.context = 32000;
    m.image_input = PROVIDER_CAP_YES;
    s = model_desc_line(&m, NULL);
    EXPECT_STR_EQ(s, "32k context");
    free(s);
}

static void test_tools_stay_off_the_gutter(void)
{
    /* Tool support dims the row and names itself in the row's detail; it
     * must not also consume a gutter segment, or every unusable model says
     * the same thing twice. */
    struct model_info m;
    model_info_init(&m);
    m.context = 32000;
    m.tools = PROVIDER_CAP_NO;

    char *s = model_desc_line(&m, NULL);
    EXPECT_STR_EQ(s, "32k context");
    free(s);

    /* And a model known ONLY to lack tools earns no gutter at all. */
    model_info_init(&m);
    m.tools = PROVIDER_CAP_NO;
    EXPECT(model_desc_line(&m, NULL) == NULL);
}

static void test_power_of_two_window(void)
{
    /* Real windows are usually powers of two (1048576, 262144). Users know
     * those as "1M" and "262k", not "1.05M" / "1048k". */
    struct model_info m;
    model_info_init(&m);
    m.context = 1048576;

    char *s = model_desc_line(&m, NULL);
    EXPECT_STR_EQ(s, "1M context");
    free(s);

    /* A window that really is fractional keeps its decimal. */
    model_info_init(&m);
    m.context = 1500000;
    s = model_desc_line(&m, NULL);
    EXPECT_STR_EQ(s, "1.5M context");
    free(s);
}

static void test_free_model(void)
{
    /* Zero rates are a real answer (free tier), distinct from unknown. */
    struct model_info m;
    model_info_init(&m);
    m.cost_input = 0;
    m.cost_output = 0;

    char *s = model_desc_line(&m, NULL);
    EXPECT_STR_EQ(s, "free");
    free(s);
}

static void test_desc_only(void)
{
    /* Prose with no metadata still earns a gutter, with no stray separator. */
    struct model_info m;
    model_info_init(&m);
    m.desc = "Latest frontier agentic coding model.";

    char *s = model_desc_line(&m, NULL);
    EXPECT_STR_EQ(s, "Latest frontier agentic coding model.");
    free(s);
}

int main(void)
{
    test_reported_full();
    test_catalog_fills_gaps();
    test_unknown_fields_omitted();
    test_image_input_only_when_absent();
    test_tools_stay_off_the_gutter();
    test_power_of_two_window();
    test_free_model();
    test_desc_only();
    T_REPORT();
}
