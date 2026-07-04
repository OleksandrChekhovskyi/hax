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
    /* A config.json with custom OpenAI-compatible providers plus an override
     * of the shipped ollama recipe. Nested object form for most, and one in
     * the flat-dotted form config.c also accepts ("flatprov"), to prove a
     * flat-defined provider is enumerable, not just readable. Loaded BEFORE
     * any registry call, since the dynamic-provider set is built once and
     * cached. */
    EXPECT(config_load("{"
                       "  \"providers\": {"
                       "    \"myllm\": {\"base_url\": \"http://127.0.0.1:9000/v1\","
                       "               \"display_name\": \"My LLM\"},"
                       "    \"bad\":   {\"api\": \"soap-1.2\","
                       "               \"base_url\": \"http://x/v1\"},"
                       "    \"claudish\": {\"api\": \"anthropic-messages\","
                       "               \"base_url\": \"http://127.0.0.1:18080/v1\","
                       "               \"sort_models\": \"on\"},"
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
    EXPECT(nk == 6);
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
    const struct provider_factory *f = provider_find("myllm");
    struct provider *p = f->new(f->name);
    EXPECT(p != NULL);
    if (p) {
        const char *const *eff = NULL;
        EXPECT_STR_EQ(p->name, "My LLM");
        EXPECT(p->list_efforts && p->list_efforts(p, &eff) > 0);
        EXPECT(p->sort_models == 0); /* sort_models unset → catalog order */
        p->destroy(p);
    }

    /* The ollama recipe opts out of the effort ladder (a local server has no
     * categorical effort), so /effort skips it rather than persist an unusable
     * value. */
    const struct provider_factory *of = provider_find("ollama");
    struct provider *op = of->new(of->name);
    EXPECT(op != NULL);
    if (op) {
        const char *const *eff = NULL;
        EXPECT(op->list_efforts(op, &eff) == 0);
        op->destroy(op);
    }

    /* An anthropic-messages provider constructs offline (no probe). Its
     * default budget thinking mode ignores effort, so /effort is hidden;
     * switching to adaptive mode advertises the ladder (low..max). */
    const struct provider_factory *af = provider_find("claudish");
    EXPECT(af != NULL);
    EXPECT(selectable("claudish"));
    struct provider *ap = af->new(af->name);
    EXPECT(ap != NULL);
    if (ap) {
        const char *const *eff = NULL;
        EXPECT_STR_EQ(ap->name, "claudish");
        /* providers.<name>.sort_models is dialect-agnostic: resolved by the
         * config-provider layer, so it reaches an anthropic-dialect provider
         * the same as an openai one. */
        EXPECT(ap->sort_models == 1);
        EXPECT(ap->list_efforts && ap->list_efforts(ap, &eff) == 0); /* budget → hidden */
        config_set_override("providers.claudish.thinking_mode", "adaptive");
        EXPECT(ap->list_efforts(ap, &eff) == 5); /* adaptive → ladder shown */
        config_set_override("providers.claudish.thinking_mode", NULL);
        ap->destroy(ap);
    }

    /* An unsupported dialect is a construction failure, not a crash. */
    const struct provider_factory *fb = provider_find("bad");
    EXPECT(fb != NULL);
    EXPECT(fb->new(fb->name) == NULL);

    config_free();
    T_REPORT();
}
