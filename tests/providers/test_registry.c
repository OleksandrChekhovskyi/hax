/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "diag.h"
#include "harness.h"
#include "provider.h"
#include "providers/anthropic_models.h"
#include "providers/http_provider.h"
#include "providers/openrouter.h"
#include "providers/registry.h"

/* Position of `name` in provider_all() (the autoselect-priority order), or -1
 * when it isn't a selectable provider. */
static int idx_of(const char *name)
{
    size_t n;
    const struct provider_def *const *all = provider_all(&n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(all[i]->id, name) == 0)
            return (int)i;
    return -1;
}

/* The default is the first (highest-priority) selectable provider. */
static void test_default_is_highest_priority(void)
{
    size_t n;
    const struct provider_def *const *all = provider_all(&n);
    EXPECT(n > 0);
    EXPECT(provider_default() == all[0]);
    EXPECT_STR_EQ(provider_default()->id, "codex");
}

/* mock is internal: excluded from the selectable set, but still resolvable
 * by name (HAX_PROVIDER=mock keeps working). */
static void test_internal_providers_hidden(void)
{
    EXPECT(idx_of("mock") == -1);
    EXPECT(provider_find("mock") != NULL);
}

static void test_gateway_defs_registered(void)
{
    EXPECT(provider_find("opencode-zen") != NULL);
    EXPECT(provider_find("opencode-go") != NULL);
    EXPECT(idx_of("opencode-zen") >
           idx_of("openrouter")); /* data defs rank after the primary built-ins */

    /* The picker names the exact variable to set, like the compiled-in providers. */
    unsetenv("OPENCODE_API_KEY");
    const struct provider_def *zen = provider_find("opencode-zen");
    struct provider_availability availability = {0};
    provider_prepare_availability(zen, &availability);
    EXPECT(!availability.available);
    EXPECT_STR_EQ(availability.reason, "OPENCODE_API_KEY not set");
    provider_availability_clear(&availability);
}

/* Autoselect-priority ordering follows the shipped table: concrete providers first, the generic
 * -compatible endpoints last so a deliberately configured concrete provider outranks a leftover
 * generic base-URL variable, and custom config blocks after every shipped def. */
static void test_autoselect_order(void)
{
    int llama = idx_of("llamacpp");
    int compat = idx_of("openai-compatible");
    int ollama = idx_of("ollama");
    EXPECT(llama >= 0 && compat >= 0 && ollama >= 0);
    EXPECT(llama < ollama);  /* shipped defs keep their table order */
    EXPECT(ollama < compat); /* generic endpoints rank last */
}

/* The former llamacpp id keeps resolving for saved sessions and scripts. */
static void test_former_id_canonicalized(void)
{
    EXPECT(provider_find("llama.cpp") == provider_find("llamacpp"));
    EXPECT(provider_find("llamacpp") != NULL);
}

/* Picker labels resolve without construction: a configured display_name wins, then the
 * factory default, then the id. Each display-name variable renames only its own provider. */
static void test_display_name_resolution(void)
{
    unsetenv("HAX_OPENAI_DISPLAY_NAME");
    EXPECT_STR_EQ(provider_display_name(provider_find("llamacpp")), "llama.cpp");
    EXPECT_STR_EQ(provider_display_name(provider_find("openai")), "openai");
    EXPECT_STR_EQ(provider_display_name(provider_find("openai-compatible")), "openai-compatible");

    setenv("HAX_OPENAI_DISPLAY_NAME", "vLLM", 1);
    EXPECT_STR_EQ(provider_display_name(provider_find("openai-compatible")), "vLLM");
    EXPECT_STR_EQ(provider_display_name(provider_find("anthropic-compatible")),
                  "anthropic-compatible");
    unsetenv("HAX_OPENAI_DISPLAY_NAME");
}

/* Capability hooks declared on a def reach the constructed provider — including the /models
 * entry parser the listing applies, not just the vtable hooks. */
static void test_def_hooks_reach_provider(void)
{
    unsetenv("HAX_MODEL");
    const struct provider_def *def = provider_find("openrouter");
    EXPECT(def != NULL);
    if (!def)
        return;
    struct provider *provider = provider_construct(def);
    EXPECT(provider != NULL);
    if (provider) {
        EXPECT(http_provider_parse_model(provider) == openrouter_parse_model);
        EXPECT(provider->probe_model == openrouter_probe_model);
        EXPECT(provider->query_usage == openrouter_query_usage);
        provider->destroy(provider);
    }

    /* Moving the metadata dialect away from the def's own stands its metadata hooks down: the
     * dialect's listing and probe stay paired instead of mixing auth schemes and shapes. */
    config_set_override("providers.openrouter.metadata_api", "anthropic");
    provider = provider_construct(def);
    EXPECT(provider != NULL);
    if (provider) {
        EXPECT(provider->probe_model == anthropic_probe_model);
        EXPECT(provider->query_usage == openrouter_query_usage); /* dialect-independent */
        provider->destroy(provider);
    }
    config_set_override("providers.openrouter.metadata_api", NULL);
}

/* The compat env aliases land in providers.openai-compatible.*; a pinned first-party def reads
 * only its own providers.openai block, so a key or name meant for a compatible endpoint cannot
 * alter it. */
static void test_pinned_def_ignores_compat_config(void)
{
    setenv("HAX_OPENAI_DISPLAY_NAME", "Renamed", 1);
    setenv("HAX_OPENAI_API_KEY", "sk-compat", 1);
    unsetenv("OPENAI_API_KEY");
    unsetenv("HAX_MODEL");

    const struct provider_def *def = provider_find("openai");
    EXPECT(def != NULL);
    if (!def)
        return;

    struct provider *provider = provider_construct(def);
    EXPECT(provider != NULL);
    if (provider) {
        EXPECT_STR_EQ(provider->name, "openai");
        provider->destroy(provider);
    }

    struct provider_availability availability = {0};
    provider_prepare_availability(def, &availability);
    EXPECT(!availability.available);
    EXPECT_STR_EQ(availability.reason, "OPENAI_API_KEY not set");
    provider_availability_clear(&availability);

    /* An inline key in the provider's own block makes it available, matching construction. */
    config_set_override("providers.openai.api_key", "sk-inline");
    provider_prepare_availability(def, &availability);
    EXPECT(availability.available);
    provider_availability_clear(&availability);
    config_set_override("providers.openai.api_key", NULL);

    /* Pinned base_url and api warn instead of silently vanishing, while a base field such as
     * sort_models is consumed. The field check lints the config file, so these arrive through
     * the file tier. */
    EXPECT(config_load("{\"providers\": {\"openai\": {\"base_url\": \"http://127.0.0.1:1\","
                       " \"api\": \"openai-completions\", \"sort_models\": \"off\"}}}") == 0);
    unsigned long diagnostics_before = hax_diag_sequence();
    provider = provider_construct(def);
    EXPECT(hax_diag_sequence() == diagnostics_before + 2);
    EXPECT(provider != NULL);
    if (provider) {
        EXPECT(provider->keep_model_order == 1); /* sort_models off → server order */
        provider->destroy(provider);
    }
    EXPECT(config_load(NULL) == 0);

    /* The provider's own block renames the banner; the provenance id stays stable. */
    config_set_override("providers.openai.display_name", "Work OpenAI");
    provider = provider_construct(def);
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

/* A local def's "{port}" base_url placeholder expands to the registered port setting: the
 * shipped default without configuration, a configured port otherwise. */
static void test_port_template(void)
{
    const struct provider_def *def = provider_find("ollama");
    EXPECT(def != NULL);
    if (!def)
        return;

    struct provider_availability availability = {0};
    provider_prepare_availability(def, &availability);
    EXPECT_STR_EQ(availability.url, "http://127.0.0.1:11434/v1/models");
    provider_availability_clear(&availability);

    config_set_override("providers.ollama.port", "18111");
    provider_prepare_availability(def, &availability);
    EXPECT_STR_EQ(availability.url, "http://127.0.0.1:18111/v1/models");
    provider_availability_clear(&availability);

    /* Malformed and out-of-range ports degrade to the def's default, never into the URL. */
    config_set_override("providers.ollama.port", "notaport");
    provider_prepare_availability(def, &availability);
    EXPECT_STR_EQ(availability.url, "http://127.0.0.1:11434/v1/models");
    provider_availability_clear(&availability);
    config_set_override("providers.ollama.port", "70000");
    provider_prepare_availability(def, &availability);
    EXPECT_STR_EQ(availability.url, "http://127.0.0.1:11434/v1/models");
    provider_availability_clear(&availability);
    config_set_override("providers.ollama.port", NULL);

    /* Construction expands the same way, and the port member is consumed by the templated
     * def rather than warned about. */
    EXPECT(config_load("{\"providers\": {\"ollama\": {\"port\": 18112}}}") == 0);
    unsigned long diagnostics_before = hax_diag_sequence();
    struct provider *ollama = provider_construct(def);
    EXPECT(hax_diag_sequence() == diagnostics_before);
    EXPECT(ollama != NULL);
    if (ollama) {
        EXPECT_STR_EQ(http_provider_base_url(ollama), "http://127.0.0.1:18112/v1");
        ollama->destroy(ollama);
    }
    EXPECT(config_load(NULL) == 0);
}

int main(void)
{
    test_default_is_highest_priority();
    test_internal_providers_hidden();
    test_gateway_defs_registered();
    test_autoselect_order();
    test_former_id_canonicalized();
    test_display_name_resolution();
    test_def_hooks_reach_provider();
    test_port_template();
    test_pinned_def_ignores_compat_config();
    T_REPORT();
}
