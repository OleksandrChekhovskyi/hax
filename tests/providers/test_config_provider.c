/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "harness.h"
#include "provider.h"
#include "providers/registry.h"

/* Whether `name` appears in provider_all() (the selectable set). */
static int selectable(const char *name)
{
    size_t n;
    const struct provider_factory *const *all = provider_all(&n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(all[i]->name, name) == 0)
            return 1;
    return 0;
}

int main(void)
{
    /* Constructing a provider starts a metadata probe for the configured
     * model, and this file mutates config while one could be in flight. The
     * providers here name no model of their own, so an ambient HAX_MODEL is
     * the only way one arrives — drop it. */
    unsetenv("HAX_MODEL");

    /* A config.json with custom OpenAI-compatible providers plus an override
     * of the shipped ollama recipe. Nested object form for most, and one in
     * the flat-dotted form config.c also accepts ("flatprov"), to prove a
     * flat-defined provider is enumerable, not just readable. Loaded BEFORE
     * any registry call, since the dynamic-provider set is built once and
     * cached. */
    EXPECT(config_load("{"
                       "  \"providers\": {"
                       "    \"myllm\": {\"base_url\": \"http://127.0.0.1:9000/v1/\","
                       "               \"display_name\": \"My LLM\"},"
                       "    \"keyed\": {\"base_url\": \"http://127.0.0.1:9004/v1\","
                       "               \"api_key_env\": \"HAX_TEST_KEYED_KEY\"},"
                       "    \"inline\": {\"base_url\": \"http://127.0.0.1:9005/v1\","
                       "               \"api_key\": \"sk-inline\"},"
                       "    \"bad\":   {\"api\": \"soap-1.2\","
                       "               \"base_url\": \"http://x/v1\"},"
                       "    \"claudish\": {\"api\": \"anthropic-messages\","
                       "               \"base_url\": \"http://127.0.0.1:18080/v1\","
                       "               \"sort_models\": \"on\","
                       "               \"catalog_id\": \"anthropic\"},"
                       "    \"nocat\": {\"base_url\": \"http://127.0.0.1:9003/v1\","
                       "               \"catalog_id\": \"\"},"
                       "    \"ollama\": {\"base_url\": \"http://gpu:1234/v1\"},"
                       "    \"my.llm\": {\"base_url\": \"http://127.0.0.1:9002/v1\"}"
                       "  },"
                       "  \"providers.flatprov.base_url\": \"http://127.0.0.1:9001/v1\""
                       "}") == 0);

    /* config_object_keys is a faithful enumerator: it returns every member
     * name across both forms, including the dotted one (filtering is the
     * provider layer's job, below). */
    char **names = NULL;
    size_t nk = config_object_keys("providers", &names);
    EXPECT(nk == 9);
    for (size_t i = 0; i < nk; i++)
        free(names[i]);
    free(names);

    /* The flat-dotted definition resolves, is selectable, and constructs. */
    EXPECT(provider_find("flatprov") != NULL);
    EXPECT(selectable("flatprov"));

    /* A dotted provider name collides with the config key path separator, so
     * its fields could never resolve — it's rejected at factory-build time
     * (with a warning), never offered as a half-working provider. */
    EXPECT(provider_find("my.llm") == NULL);
    EXPECT(!selectable("my.llm"));

    /* Config-defined providers resolve by name and show up as selectable,
     * alongside the shipped recipe and the compiled-in factories. */
    EXPECT(provider_find("myllm") != NULL);
    EXPECT(selectable("myllm"));
    EXPECT(provider_find("ollama") != NULL); /* recipe (overlaid by config) */
    EXPECT(selectable("ollama"));
    EXPECT(provider_find("codex") != NULL); /* still a compiled-in factory */
    EXPECT(provider_find("does-not-exist") == NULL);

    /* The recipe name appears once, not twice, when a config block overlays
     * it: provider_find returns a single factory and it isn't a duplicate of
     * any other. */
    size_t n;
    const struct provider_factory *const *all = provider_all(&n);
    int ollama_count = 0;
    for (size_t i = 0; i < n; i++)
        if (strcmp(all[i]->name, "ollama") == 0)
            ollama_count++;
    EXPECT(ollama_count == 1);

    /* Construction of an openai-completions provider succeeds offline (no
     * probe) and takes its banner from the resolved display_name. A generic
     * config provider offers the advisory effort ladder. */
    const struct provider_factory *myllm_factory = provider_find("myllm");
    /* Availability captures an owned request before a worker is spawned; the
     * configured trailing slash is trimmed before "/models" is appended. */
    struct provider_availability probe = {0};
    myllm_factory->prepare_availability(myllm_factory->name, &probe);
    EXPECT_STR_EQ(probe.url, "http://127.0.0.1:9000/v1/models");
    config_set_override("providers.myllm.base_url", "http://changed/v1");
    EXPECT_STR_EQ(probe.url, "http://127.0.0.1:9000/v1/models");
    config_set_override("providers.myllm.base_url", NULL);
    provider_availability_clear(&probe);

    /* A keyed provider's availability is its key resolving — no probe request:
     * unavailable while the declared env var is unset, available once set. */
    const struct provider_factory *keyed_factory = provider_find("keyed");
    unsetenv("HAX_TEST_KEYED_KEY");
    struct provider_availability keyed = {0};
    keyed_factory->prepare_availability(keyed_factory->name, &keyed);
    EXPECT(!keyed.available);
    EXPECT_STR_EQ(keyed.reason, "API key not set");
    EXPECT(keyed.url == NULL);
    setenv("HAX_TEST_KEYED_KEY", "sk-keyed", 1);
    keyed_factory->prepare_availability(keyed_factory->name, &keyed);
    EXPECT(keyed.available);
    EXPECT(keyed.url == NULL);
    unsetenv("HAX_TEST_KEYED_KEY");

    /* An inline api_key keys the provider all by itself. */
    const struct provider_factory *inline_factory = provider_find("inline");
    struct provider_availability inline_avail = {0};
    inline_factory->prepare_availability(inline_factory->name, &inline_avail);
    EXPECT(inline_avail.available);
    EXPECT(inline_avail.url == NULL);

    struct provider *myllm = myllm_factory->new(myllm_factory->name);
    EXPECT(myllm != NULL);
    if (myllm) {
        const char *const *efforts = NULL;
        EXPECT_STR_EQ(myllm->name, "My LLM");
        EXPECT(myllm->list_efforts && myllm->list_efforts(myllm, &efforts) > 0);
        EXPECT(myllm->sort_models == 0); /* sort_models unset → catalog order */
        /* catalog_id defaults to the provider's own name. */
        EXPECT_STR_EQ(myllm->catalog_id, "myllm");
        myllm->destroy(myllm);
    }

    /* An explicit empty catalog_id opts out of catalog lookups. */
    const struct provider_factory *nocat_factory = provider_find("nocat");
    struct provider *nocat = nocat_factory->new(nocat_factory->name);
    EXPECT(nocat != NULL);
    if (nocat) {
        EXPECT(nocat->catalog_id == NULL);
        nocat->destroy(nocat);
    }

    /* The ollama recipe opts out of the effort ladder, and its curated
     * catalog_id absence is final — no fallback to the provider name. */
    const struct provider_factory *ollama_factory = provider_find("ollama");
    struct provider *ollama = ollama_factory->new(ollama_factory->name);
    EXPECT(ollama != NULL);
    if (ollama) {
        const char *const *efforts = NULL;
        EXPECT(ollama->list_efforts(ollama, &efforts) == 0);
        EXPECT(ollama->catalog_id == NULL);
        ollama->destroy(ollama);
    }

    const struct provider_factory *anthropic_factory = provider_find("claudish");
    EXPECT(anthropic_factory != NULL);
    EXPECT(selectable("claudish"));
    struct provider *anthropic = anthropic_factory->new(anthropic_factory->name);
    EXPECT(anthropic != NULL);
    if (anthropic) {
        const char *const *efforts = NULL;
        EXPECT_STR_EQ(anthropic->name, "claudish");
        EXPECT_STR_EQ(anthropic->catalog_id, "anthropic");
        EXPECT(anthropic->sort_models == 1);
        EXPECT(anthropic->list_efforts && anthropic->list_efforts(anthropic, &efforts) == 0);
        config_set_override("providers.claudish.thinking_mode", "adaptive");
        EXPECT(anthropic->list_efforts(anthropic, &efforts) == 5);
        config_set_override("providers.claudish.thinking_mode", NULL);
        anthropic->destroy(anthropic);
    }

    /* An unsupported dialect is a construction failure, not a crash. */
    const struct provider_factory *bad_factory = provider_find("bad");
    EXPECT(bad_factory != NULL);
    EXPECT(bad_factory->new(bad_factory->name) == NULL);

    config_free();
    T_REPORT();
}
