/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "harness.h"
#include "provider.h"
#include "util.h"
#include "providers/config_provider.h"
#include "providers/registry.h"

/* The env-alias rows registered in config.c for the shipped -compatible blocks must project
 * the provider field inventory: same leaves, same dialect, same secrecy. */
static void expect_registry_projects_provider_fields(void)
{
    static const struct {
        const char *prefix;
        unsigned dialect;
    } BLOCKS[] = {
        {.prefix = "providers.openai-compatible.", .dialect = PROVIDER_FIELD_OPENAI},
        {.prefix = "providers.anthropic-compatible.", .dialect = PROVIDER_FIELD_ANTHROPIC},
    };

    size_t n_fields = 0;
    const struct provider_field *fields = provider_fields(&n_fields);
    size_t n_settings = 0;
    const struct config_setting *settings = config_settings(&n_settings);

    for (size_t i = 0; i < n_settings; i++) {
        for (size_t b = 0; b < 2; b++) {
            size_t prefix_length = strlen(BLOCKS[b].prefix);
            if (strncmp(settings[i].key, BLOCKS[b].prefix, prefix_length) != 0)
                continue;

            const char *leaf = settings[i].key + prefix_length;
            const struct provider_field *field = NULL;
            for (size_t f = 0; f < n_fields; f++) {
                if (strcmp(fields[f].leaf, leaf) == 0)
                    field = &fields[f];
            }
            EXPECT(field != NULL);
            if (field) {
                EXPECT(field->dialects & BLOCKS[b].dialect);
                EXPECT(field->secret == settings[i].secret);
            }
        }
    }
}

static void test_cache_ttl_resolution(void)
{
    /* No config namespace, unset, and empty all take the interactive-agent default. */
    EXPECT_STR_EQ(provider_cache_ttl(NULL), "1h");
    EXPECT_STR_EQ(provider_cache_ttl("providers.ttltest"), "1h");

    config_set_override("providers.ttltest.cache_ttl", "5m");
    EXPECT_STR_EQ(provider_cache_ttl("providers.ttltest"), "5m");
    config_set_override("providers.ttltest.cache_ttl", "1H"); /* canonicalized */
    EXPECT_STR_EQ(provider_cache_ttl("providers.ttltest"), "1h");
    config_set_override("providers.ttltest.cache_ttl", "2h"); /* warns; falls back */
    EXPECT_STR_EQ(provider_cache_ttl("providers.ttltest"), "1h");
    config_set_override("providers.ttltest.cache_ttl", NULL);
}

/* extra_body survives config-load scalar normalization with its JSON types intact, drops
 * protocol-owned members with one warning each, and merges over a built body recursively. */
static void test_extra_body(void)
{
    unsigned long diagnostics_before = hax_diag_sequence();
    json_t *extra = provider_extra_body("providers.extras");
    /* Reserved 'model', 'n', 'system', and 'include' warn and drop. */
    EXPECT(hax_diag_sequence() == diagnostics_before + 4);
    EXPECT(extra != NULL);
    if (!extra)
        return;

    EXPECT(json_object_get(extra, "model") == NULL);
    EXPECT(json_object_get(extra, "n") == NULL);
    EXPECT(json_object_get(extra, "system") == NULL);
    EXPECT(json_object_get(extra, "include") == NULL);
    EXPECT(json_is_real(json_object_get(extra, "temperature")));
    EXPECT(json_real_value(json_object_get(extra, "temperature")) == 0.25);
    EXPECT(json_is_integer(json_object_get(extra, "top_logprobs")));
    json_t *routing = json_object_get(extra, "provider");
    EXPECT(json_is_false(json_object_get(routing, "allow_fallbacks")));
    EXPECT(json_is_array(json_object_get(routing, "order")));

    /* A member overrides the built field; an object member extends a built object; members
     * the extra body never mentions survive. */
    json_t *body = json_pack("{s:s, s:f, s:{s:s}}", "model", "m", "temperature", 1.0, "provider",
                             "sort", "price");
    provider_extra_body_apply(body, extra);
    EXPECT_STR_EQ(json_string_value(json_object_get(body, "model")), "m");
    EXPECT(json_real_value(json_object_get(body, "temperature")) == 0.25);
    json_t *merged = json_object_get(body, "provider");
    EXPECT_STR_EQ(json_string_value(json_object_get(merged, "sort")), "price");
    EXPECT(json_is_array(json_object_get(merged, "order")));
    json_decref(body);
    json_decref(extra);

    /* The flat-dotted spelling is exempt from normalization too. */
    json_t *flat = provider_extra_body("providers.flatprov");
    EXPECT(flat != NULL);
    if (flat) {
        EXPECT(json_is_real(json_object_get(flat, "top_p")));
        json_decref(flat);
    }

    /* Raw types are a property of the structured read: the same numeric scalar read as a
     * string setting coerces like any other value. */
    EXPECT_STR_EQ(config_str("extra_body.stray"), "5");

    /* A non-object value warns and resolves to nothing; absence stays silent. */
    diagnostics_before = hax_diag_sequence();
    EXPECT(provider_extra_body("providers.keyed") == NULL);
    EXPECT(hax_diag_sequence() == diagnostics_before + 1);
    EXPECT(provider_extra_body("providers.myllm") == NULL);
    EXPECT(provider_extra_body(NULL) == NULL);
    EXPECT(hax_diag_sequence() == diagnostics_before + 1);
}

/* extra_headers become "Name: value" strings; a "$NAME" value reads the environment. A
 * non-token name (space, separator), a non-string value, an unset variable, an empty value
 * (curl would suppress the header instead of sending it empty), and a control character
 * (literal, DEL, or smuggled through a variable) each warn and drop. */
static void test_extra_headers(void)
{
    setenv("HAX_TEST_HEADER", "from-env", 1);
    setenv("HAX_TEST_EVIL_HEADER", "a\r\nX-Smuggled: gotcha", 1);
    unsetenv("HAX_TEST_UNSET_HEADER");

    unsigned long diagnostics_before = hax_diag_sequence();
    char **headers = provider_extra_headers("providers.extras");
    EXPECT(hax_diag_sequence() == diagnostics_before + 9);
    EXPECT(headers != NULL);
    if (!headers)
        return;

    size_t n_headers = 0;
    int saw_config = 0, saw_env = 0, saw_dollar = 0;
    for (char **header = headers; *header; header++) {
        n_headers++;
        if (strcmp(*header, "X-Portkey-Config: pc-1") == 0)
            saw_config = 1;
        if (strcmp(*header, "X-From-Env: from-env") == 0)
            saw_env = 1;
        if (strcmp(*header, "X-Dollar: $plain") == 0)
            saw_dollar = 1;
    }
    EXPECT(n_headers == 3);
    EXPECT(saw_config && saw_env && saw_dollar);
    string_array_free(headers);

    EXPECT(provider_extra_headers("providers.myllm") == NULL);
    EXPECT(provider_extra_headers(NULL) == NULL);
    unsetenv("HAX_TEST_HEADER");
    unsetenv("HAX_TEST_EVIL_HEADER");
}

/* An inline api_key accepts the same "$NAME" indirection, falling through to api_key_env
 * when the variable is unset; "$$" escapes a literal leading dollar. */
static void test_api_key_env_escape(void)
{
    unsetenv("HAX_TEST_DOLLAR_KEY");
    config_set_override("providers.dollartest.api_key", "$HAX_TEST_DOLLAR_KEY");
    EXPECT(provider_api_key("providers.dollartest", NULL) == NULL);

    setenv("HAX_TEST_FALLBACK_KEY", "sk-fallback", 1);
    EXPECT_STR_EQ(provider_api_key("providers.dollartest", "HAX_TEST_FALLBACK_KEY"), "sk-fallback");
    setenv("HAX_TEST_DOLLAR_KEY", "sk-dollar", 1);
    EXPECT_STR_EQ(provider_api_key("providers.dollartest", "HAX_TEST_FALLBACK_KEY"), "sk-dollar");

    config_set_override("providers.dollartest.api_key", "$$literal");
    EXPECT_STR_EQ(provider_api_key("providers.dollartest", NULL), "$literal");

    config_set_override("providers.dollartest.api_key", NULL);
    unsetenv("HAX_TEST_DOLLAR_KEY");
    unsetenv("HAX_TEST_FALLBACK_KEY");
}

/* Whether `name` appears in provider_all() (the selectable set). */
static int selectable(const char *name)
{
    size_t n;
    const struct provider_factory *const *all = provider_all(&n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(all[i]->id, name) == 0)
            return 1;
    return 0;
}

/* The config under test: custom providers in the nested object form, one in the flat-dotted
 * form config.c also accepts ("flatprov") to prove a flat-defined provider is enumerable and
 * not just readable, an override of the shipped ollama recipe, and the extra_body /
 * extra_headers fixtures the tests above read. */
static const char CONFIG_JSON[] =
    "{"
    "  \"providers\": {"
    "    \"myllm\": {\"base_url\": \"http://127.0.0.1:9000/v1/\", \"display_name\": \"My LLM\"},"
    "    \"extras\": {"
    "      \"base_url\": \"http://127.0.0.1:9009/v1\","
    "      \"extra_body\": {"
    "        \"temperature\": 0.25,"
    "        \"top_logprobs\": 3,"
    "        \"model\": \"reserved\","
    "        \"n\": 2,"
    "        \"system\": \"reserved\","
    "        \"include\": [\"b\", \"a\"],"
    "        \"provider\": {\"order\": [\"baseten\"], \"allow_fallbacks\": false}"
    "      },"
    "      \"extra_headers\": {"
    "        \"X-Portkey-Config\": \"pc-1\","
    "        \"X-From-Env\": \"$HAX_TEST_HEADER\","
    "        \"X-Dollar\": \"$$plain\","
    "        \"X-Unset\": \"$HAX_TEST_UNSET_HEADER\","
    "        \"X-Evil\": \"$HAX_TEST_EVIL_HEADER\","
    "        \"X-Num\": 7,"
    "        \"Bad Name\": \"x\","
    "        \"X@Host\": \"x\","
    "        \"X-Ctl\": \"a\\nb\","
    "        \"X-Del\": \"a\\u007Fb\","
    "        \"X-Empty\": \"\","
    "        \"X-Obj\": {\"no\": 1}"
    "      }"
    "    },"
    "    \"keyed\": {\"base_url\": \"http://127.0.0.1:9004/v1\","
    "               \"api_key_env\": \"HAX_TEST_KEYED_KEY\"},"
    "    \"inline\": {\"base_url\": \"http://127.0.0.1:9005/v1\", \"api_key\": \"sk-inline\"},"
    "    \"bad\": {\"api\": \"soap-1.2\", \"base_url\": \"http://x/v1\"},"
    "    \"respprov\": {\"api\": \"Responses\", \"base_url\": \"http://127.0.0.1:9006/v1\"},"
    "    \"claudish\": {\"api\": \"anthropic-messages\","
    "                  \"base_url\": \"http://127.0.0.1:18080/v1\","
    "                  \"sort_models\": \"on\","
    "                  \"catalog_id\": \"anthropic\"},"
    "    \"nocat\": {\"base_url\": \"http://127.0.0.1:9003/v1\", \"catalog_id\": \"\"},"
    "    \"ollama\": {\"base_url\": \"http://gpu:1234/v1/\","
    "                 \"extra_headers\": {\"X-Local\": \"ok\"}},"
    "    \"warny\": {\"base_url\": \"http://127.0.0.1:9008/v1\","
    "               \"resoning_format\": \"nested\","
    "               \"thinking_mode\": \"budget\"},"
    "    \"my.llm\": {\"base_url\": \"http://127.0.0.1:9002/v1\"}"
    "  },"
    "  \"providers.flatprov.base_url\": \"http://127.0.0.1:9001/v1\","
    "  \"providers.flatprov.extra_body\": {\"top_p\": 0.9},"
    "  \"providers.keyed.extra_body\": \"not an object\","
    "  \"extra_body\": {\"stray\": 5}"
    "}";

int main(void)
{
    /* Constructing a provider starts a metadata probe for the configured
     * model, and this file mutates config while one could be in flight. The
     * providers here name no model of their own, so an ambient HAX_MODEL is
     * the only way one arrives — drop it. The compat env aliases would
     * configure the shipped -compatible recipes tested below. */
    unsetenv("HAX_MODEL");
    unsetenv("HAX_OPENAI_BASE_URL");
    unsetenv("HAX_OPENAI_API_KEY");
    unsetenv("HAX_ANTHROPIC_BASE_URL");
    unsetenv("HAX_OPENAI_DISPLAY_NAME");

    /* Loaded BEFORE any registry call, since the dynamic-provider set is built once and
     * cached. */
    EXPECT(config_load(CONFIG_JSON) == 0);

    /* config_object_keys is a faithful enumerator: it returns every member
     * name across both forms, including the dotted one (filtering is the
     * provider layer's job, below). */
    char **names = NULL;
    size_t nk = config_object_keys("providers", &names);
    EXPECT(nk == 12);
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
        if (strcmp(all[i]->id, "ollama") == 0)
            ollama_count++;
    EXPECT(ollama_count == 1);

    /* Construction of an openai-completions provider succeeds offline (no
     * probe) and takes its banner from the resolved display_name. A generic
     * config provider offers the advisory effort ladder. */
    const struct provider_factory *myllm_factory = provider_find("myllm");
    /* A keyless custom provider counts its configured base_url as availability: a generic
     * endpoint may not serve the /models route a probe would need. */
    struct provider_availability probe = {0};
    myllm_factory->prepare_availability(myllm_factory->id, &probe);
    EXPECT(probe.available);
    EXPECT(probe.url == NULL);
    provider_availability_clear(&probe);

    /* The ollama recipe opts into a reachability probe. It captures an owned request before a
     * worker is spawned; the configured trailing slash is trimmed before "/models" is
     * appended, and configured extra_headers ride along. */
    const struct provider_factory *probe_factory = provider_find("ollama");
    probe_factory->prepare_availability(probe_factory->id, &probe);
    EXPECT_STR_EQ(probe.url, "http://gpu:1234/v1/models");
    EXPECT(probe.headers != NULL);
    if (probe.headers)
        EXPECT_STR_EQ(probe.headers[0], "X-Local: ok");
    config_set_override("providers.ollama.base_url", "http://changed/v1");
    EXPECT_STR_EQ(probe.url, "http://gpu:1234/v1/models");
    config_set_override("providers.ollama.base_url", NULL);
    provider_availability_clear(&probe);

    /* A keyed provider's availability is its key resolving — no probe request:
     * unavailable while the declared env var is unset, available once set. */
    const struct provider_factory *keyed_factory = provider_find("keyed");
    unsetenv("HAX_TEST_KEYED_KEY");
    struct provider_availability keyed = {0};
    keyed_factory->prepare_availability(keyed_factory->id, &keyed);
    EXPECT(!keyed.available);
    EXPECT_STR_EQ(keyed.reason, "API key not set");
    EXPECT(keyed.url == NULL);
    setenv("HAX_TEST_KEYED_KEY", "sk-keyed", 1);
    keyed_factory->prepare_availability(keyed_factory->id, &keyed);
    EXPECT(keyed.available);
    EXPECT(keyed.url == NULL);
    unsetenv("HAX_TEST_KEYED_KEY");

    /* An inline api_key keys the provider all by itself. */
    const struct provider_factory *inline_factory = provider_find("inline");
    struct provider_availability inline_avail = {0};
    inline_factory->prepare_availability(inline_factory->id, &inline_avail);
    EXPECT(inline_avail.available);
    EXPECT(inline_avail.url == NULL);

    struct provider *myllm = myllm_factory->new(myllm_factory->id);
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
    struct provider *nocat = nocat_factory->new(nocat_factory->id);
    EXPECT(nocat != NULL);
    if (nocat) {
        EXPECT(nocat->catalog_id == NULL);
        nocat->destroy(nocat);
    }

    /* The ollama recipe opts out of the effort ladder, and its curated
     * catalog_id absence is final — no fallback to the provider name. */
    const struct provider_factory *ollama_factory = provider_find("ollama");
    struct provider *ollama = ollama_factory->new(ollama_factory->id);
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
    struct provider *anthropic = anthropic_factory->new(anthropic_factory->id);
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

    /* The Responses dialect is a supported api value, not an unknown one. */
    const struct provider_factory *resp_factory = provider_find("respprov");
    EXPECT(resp_factory != NULL);
    struct provider *respprov = resp_factory->new(resp_factory->id);
    EXPECT(respprov != NULL);
    if (respprov)
        respprov->destroy(respprov);

    /* An unsupported dialect is a construction failure, not a crash. */
    const struct provider_factory *bad_factory = provider_find("bad");
    EXPECT(bad_factory != NULL);
    EXPECT(bad_factory->new(bad_factory->id) == NULL);

    /* A misspelled or wrong-dialect field warns (one diagnostic each) but never blocks
     * construction, so a config written for a newer hax still runs. */
    const struct provider_factory *warny_factory = provider_find("warny");
    EXPECT(warny_factory != NULL);
    unsigned long diagnostics_before = hax_diag_sequence();
    struct provider *warny = warny_factory->new(warny_factory->id);
    EXPECT(hax_diag_sequence() == diagnostics_before + 2);
    EXPECT(warny != NULL);
    if (warny)
        warny->destroy(warny);

    /* A clean block constructs without diagnostics. */
    const struct provider_factory *clean_factory = provider_find("respprov");
    diagnostics_before = hax_diag_sequence();
    struct provider *clean = clean_factory->new(clean_factory->id);
    EXPECT(hax_diag_sequence() == diagnostics_before);
    EXPECT(clean != NULL);
    if (clean)
        clean->destroy(clean);

    /* The shipped -compatible recipes have no default base_url: unavailable
     * (with a pointer at their env alias) and unconstructable until the user
     * supplies one. */
    const struct provider_factory *compat = provider_find("openai-compatible");
    EXPECT(compat != NULL);
    EXPECT(selectable("openai-compatible"));
    EXPECT(selectable("anthropic-compatible"));
    struct provider_availability compat_avail = {0};
    compat->prepare_availability(compat->id, &compat_avail);
    EXPECT(!compat_avail.available);
    EXPECT_STR_EQ(compat_avail.reason, "HAX_OPENAI_BASE_URL not set");
    EXPECT(compat->new(compat->id) == NULL);

    /* With a base_url the recipe is available without probing — the endpoint may serve only
     * its completion route. An api_key (here via its env alias) keys it, and display_name
     * (env alias HAX_OPENAI_DISPLAY_NAME) labels the banner. No catalog identity: an
     * arbitrary endpoint's models are not a hosted vendor's. */
    setenv("HAX_OPENAI_BASE_URL", "http://127.0.0.1:9007/v1/", 1);
    compat->prepare_availability(compat->id, &compat_avail);
    EXPECT(compat_avail.available);
    EXPECT(compat_avail.url == NULL);
    provider_availability_clear(&compat_avail);

    setenv("HAX_OPENAI_API_KEY", "sk-compat", 1);
    compat->prepare_availability(compat->id, &compat_avail);
    EXPECT(compat_avail.available);
    EXPECT(compat_avail.url == NULL);

    setenv("HAX_OPENAI_DISPLAY_NAME", "vLLM", 1);
    struct provider *compat_provider = compat->new(compat->id);
    EXPECT(compat_provider != NULL);
    if (compat_provider) {
        EXPECT_STR_EQ(compat_provider->name, "vLLM");
        EXPECT_STR_EQ(compat_provider->id, "openai-compatible");
        EXPECT(compat_provider->catalog_id == NULL);
        compat_provider->destroy(compat_provider);
    }
    unsetenv("HAX_OPENAI_DISPLAY_NAME");
    unsetenv("HAX_OPENAI_API_KEY");
    unsetenv("HAX_OPENAI_BASE_URL");

    expect_registry_projects_provider_fields();
    test_cache_ttl_resolution();
    test_extra_body();
    test_extra_headers();
    test_api_key_env_escape();

    config_free();
    T_REPORT();
}
