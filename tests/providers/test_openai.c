/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "config.h"
#include "harness.h"
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

int main(void)
{
    test_first_party_ignores_compat_config();
    T_REPORT();
}
