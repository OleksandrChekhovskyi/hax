/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "diag.h"
#include "harness.h"
#include "provider.h"
#include "util.h"
#include "providers/opencode.h"
#include "providers/provider_config.h"
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
                /* Key fields belong to the keyed class rather than a wire dialect; both
                 * -compatible defs are keyed and unpinned, so their aliases still project. */
                EXPECT(field->classes &
                       (BLOCKS[b].dialect | PROVIDER_FIELD_KEYED | PROVIDER_FIELD_UNPINNED));
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

/* extra_body survives config-load scalar normalization with its JSON types intact and drops
 * protocol-owned members with one warning each. */
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
    const struct provider_def *const *all = provider_all(&n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(all[i]->id, name) == 0)
            return 1;
    return 0;
}

/* The config under test: custom providers in the nested object form, one in the flat-dotted
 * form config.c also accepts ("flatprov") to prove a flat-defined provider is enumerable and
 * not just readable, an override of the shipped ollama def, and the extra_body /
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
    "                  \"sort_models\": \"off\","
    "                  \"catalog_id\": \"anthropic\"},"
    "    \"nocat\": {\"base_url\": \"http://127.0.0.1:9003/v1\", \"catalog_id\": \"\"},"
    "    \"ollama\": {\"base_url\": \"http://gpu:1234/v1/\","
    "                 \"extra_headers\": {\"X-Local\": \"ok\"}},"
    "    \"warny\": {\"base_url\": \"http://127.0.0.1:9008/v1\","
    "               \"resoning_format\": \"nested\","
    "               \"thinking_mode\": \"budget\"},"
    "    \"mixed\": {\"base_url\": \"http://127.0.0.1:9009/v1\","
    "               \"model_apis\": {\"claude-*\": \"anthropic-messages\"},"
    "               \"thinking_mode\": \"budget\","
    "               \"reasoning_format\": \"nested\"},"
    "    \"catgw\": {\"base_url\": \"http://127.0.0.1:9010/v1\","
    "               \"api\": \"catalog\","
    "               \"catalog_id\": \"opencode\","
    "               \"thinking_mode\": \"budget\","
    "               \"reasoning_format\": \"nested\"},"
    "    \"metaproxy\": {\"base_url\": \"http://127.0.0.1:9011/v1\","
    "                   \"metadata_api\": \"anthropic\","
    "                   \"version\": \"2024-02-01\"},"
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
     * configure the shipped -compatible defs tested below. */
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
    EXPECT(nk == 15);
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
     * alongside every shipped def. */
    EXPECT(provider_find("myllm") != NULL);
    EXPECT(selectable("myllm"));
    EXPECT(provider_find("ollama") != NULL); /* shipped def (overlaid by config) */
    EXPECT(selectable("ollama"));
    EXPECT(provider_find("codex") != NULL); /* still a compiled-in factory */
    EXPECT(provider_find("does-not-exist") == NULL);

    /* A shipped def's name appears once, not twice, when a config block overlays
     * it: provider_find returns a single factory and it isn't a duplicate of
     * any other. */
    size_t n;
    const struct provider_def *const *all = provider_all(&n);
    int ollama_count = 0;
    for (size_t i = 0; i < n; i++)
        if (strcmp(all[i]->id, "ollama") == 0)
            ollama_count++;
    EXPECT(ollama_count == 1);

    /* Construction of an openai-completions provider succeeds offline (no
     * probe) and takes its banner from the resolved display_name. A generic
     * config provider offers the advisory effort ladder. */
    const struct provider_def *myllm_factory = provider_find("myllm");
    /* A keyless custom provider counts its configured base_url as availability: a generic
     * endpoint may not serve the /models route a probe would need. */
    struct provider_availability probe = {0};
    provider_prepare_availability(myllm_factory, &probe);
    EXPECT(probe.available);
    EXPECT(probe.url == NULL);
    provider_availability_clear(&probe);

    /* The ollama def opts into a reachability probe. It captures an owned request before a
     * worker is spawned; the configured trailing slash is trimmed before "/models" is
     * appended, and configured extra_headers ride along. */
    const struct provider_def *probe_factory = provider_find("ollama");
    provider_prepare_availability(probe_factory, &probe);
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
    const struct provider_def *keyed_factory = provider_find("keyed");
    unsetenv("HAX_TEST_KEYED_KEY");
    struct provider_availability keyed = {0};
    provider_prepare_availability(keyed_factory, &keyed);
    EXPECT(!keyed.available);
    EXPECT_STR_EQ(keyed.reason, "HAX_TEST_KEYED_KEY not set");
    EXPECT(keyed.url == NULL);
    provider_availability_clear(&keyed);
    setenv("HAX_TEST_KEYED_KEY", "sk-keyed", 1);
    provider_prepare_availability(keyed_factory, &keyed);
    EXPECT(keyed.available);
    EXPECT(keyed.url == NULL);
    unsetenv("HAX_TEST_KEYED_KEY");

    /* An inline api_key keys the provider all by itself. */
    const struct provider_def *inline_factory = provider_find("inline");
    struct provider_availability inline_avail = {0};
    provider_prepare_availability(inline_factory, &inline_avail);
    EXPECT(inline_avail.available);
    EXPECT(inline_avail.url == NULL);

    struct provider *myllm = provider_construct(myllm_factory);
    EXPECT(myllm != NULL);
    if (myllm) {
        const char *const *efforts = NULL;
        EXPECT_STR_EQ(myllm->name, "My LLM");
        EXPECT(myllm->list_efforts && myllm->list_efforts(myllm, &efforts) > 0);
        EXPECT(myllm->keep_model_order == 0); /* sort_models unset → sorted picker */
        /* catalog_id defaults to the provider's own name. */
        EXPECT_STR_EQ(myllm->catalog_id, "myllm");
        myllm->destroy(myllm);
    }

    /* An explicit empty catalog_id opts out of catalog lookups. */
    const struct provider_def *nocat_factory = provider_find("nocat");
    struct provider *nocat = provider_construct(nocat_factory);
    EXPECT(nocat != NULL);
    if (nocat) {
        EXPECT(nocat->catalog_id == NULL);
        nocat->destroy(nocat);
    }

    /* The ollama def opts out of the effort ladder, and its curated
     * catalog_id absence is final — no fallback to the provider name. */
    const struct provider_def *ollama_factory = provider_find("ollama");
    struct provider *ollama = provider_construct(ollama_factory);
    EXPECT(ollama != NULL);
    if (ollama) {
        const char *const *efforts = NULL;
        EXPECT(ollama->list_efforts(ollama, &efforts) == 0);
        EXPECT(ollama->catalog_id == NULL);
        ollama->destroy(ollama);
    }

    /* The opencode-go def carries the /usage hook onto its provider; the Zen sibling has
     * none, so /usage stays unsupported there. */
    const struct provider_def *go_factory = provider_find("opencode-go");
    EXPECT(go_factory != NULL);
    struct provider *go = provider_construct(go_factory);
    EXPECT(go != NULL);
    if (go) {
        EXPECT(go->query_usage == opencode_go_query_usage);
        go->destroy(go);
    }
    const struct provider_def *zen_factory = provider_find("opencode-zen");
    EXPECT(zen_factory != NULL);
    struct provider *zen = provider_construct(zen_factory);
    EXPECT(zen != NULL);
    if (zen) {
        EXPECT(zen->query_usage == NULL);
        zen->destroy(zen);
    }

    /* Usage auth stays Bearer even when an api override routes the gateway's models through
     * the Messages dialect, whose model requests authenticate with x-api-key. */
    setenv("OPENCODE_API_KEY", "oc-test-key", 1);
    config_set_override("providers.opencode-go.api", "anthropic-messages");
    struct provider *go_messages = provider_construct(go_factory);
    EXPECT(go_messages != NULL);
    if (go_messages) {
        char **usage_headers = opencode_usage_headers(go_messages);
        EXPECT(usage_headers && usage_headers[0]);
        if (usage_headers && usage_headers[0])
            EXPECT_STR_EQ(usage_headers[0], "Authorization: Bearer oc-test-key");
        string_array_free(usage_headers);
        go_messages->destroy(go_messages);
    }
    config_set_override("providers.opencode-go.api", NULL);
    unsetenv("OPENCODE_API_KEY");

    const struct provider_def *anthropic_factory = provider_find("claudish");
    EXPECT(anthropic_factory != NULL);
    EXPECT(selectable("claudish"));
    struct provider *anthropic = provider_construct(anthropic_factory);
    EXPECT(anthropic != NULL);
    if (anthropic) {
        const char *const *efforts = NULL;
        EXPECT_STR_EQ(anthropic->name, "claudish");
        EXPECT_STR_EQ(anthropic->catalog_id, "anthropic");
        EXPECT(anthropic->keep_model_order == 1); /* sort_models off → server order */
        /* The unconfigured budget default upgrades to adaptive per request when an effort is
         * chosen, so the ladder stays selectable; an explicit budget pin hides it. */
        EXPECT(anthropic->list_efforts && anthropic->list_efforts(anthropic, &efforts) == 6);
        config_set_override("providers.claudish.thinking_mode", "budget");
        EXPECT(anthropic->list_efforts(anthropic, &efforts) == 0);
        config_set_override("providers.claudish.thinking_mode", "adaptive");
        EXPECT(anthropic->list_efforts(anthropic, &efforts) == 6);
        config_set_override("providers.claudish.thinking_mode", NULL);
        anthropic->destroy(anthropic);
    }

    /* The Responses dialect is a supported api value, not an unknown one. */
    const struct provider_def *resp_factory = provider_find("respprov");
    EXPECT(resp_factory != NULL);
    struct provider *respprov = provider_construct(resp_factory);
    EXPECT(respprov != NULL);
    if (respprov)
        respprov->destroy(respprov);

    /* An unsupported dialect is a construction failure, not a crash. */
    const struct provider_def *bad_factory = provider_find("bad");
    EXPECT(bad_factory != NULL);
    EXPECT(provider_construct(bad_factory) == NULL);

    /* A misspelled or wrong-dialect field warns (one diagnostic each) but never blocks
     * construction, so a config written for a newer hax still runs. */
    const struct provider_def *warny_factory = provider_find("warny");
    EXPECT(warny_factory != NULL);
    unsigned long diagnostics_before = hax_diag_sequence();
    struct provider *warny = provider_construct(warny_factory);
    EXPECT(hax_diag_sequence() == diagnostics_before + 2);
    EXPECT(warny != NULL);
    if (warny)
        warny->destroy(warny);

    /* A clean block constructs without diagnostics. */
    const struct provider_def *clean_factory = provider_find("respprov");
    diagnostics_before = hax_diag_sequence();
    struct provider *clean = provider_construct(clean_factory);
    EXPECT(hax_diag_sequence() == diagnostics_before);
    EXPECT(clean != NULL);
    if (clean)
        clean->destroy(clean);

    /* model_apis makes a chat provider a mixed-protocol gateway, so fields from every dialect
     * are live and must not draw "not used" warnings. */
    const struct provider_def *mixed_factory = provider_find("mixed");
    EXPECT(mixed_factory != NULL);
    diagnostics_before = hax_diag_sequence();
    struct provider *mixed = provider_construct(mixed_factory);
    EXPECT(hax_diag_sequence() == diagnostics_before);
    EXPECT(mixed != NULL);
    if (mixed)
        mixed->destroy(mixed);

    /* An OpenAI-wire provider fronting an Anthropic-shaped /models legitimately consumes
     * `version` for its metadata headers: no "unused field" warning. */
    const struct provider_def *metaproxy_def = provider_find("metaproxy");
    EXPECT(metaproxy_def != NULL);
    diagnostics_before = hax_diag_sequence();
    struct provider *metaproxy = provider_construct(metaproxy_def);
    EXPECT(hax_diag_sequence() == diagnostics_before);
    EXPECT(metaproxy != NULL);
    if (metaproxy)
        metaproxy->destroy(metaproxy);

    /* api "catalog" is the rule-free gateway opt-in: routing comes from catalog hints alone,
     * and it too must construct with every dialect's fields and no diagnostics. */
    const struct provider_def *catgw_factory = provider_find("catgw");
    EXPECT(catgw_factory != NULL);
    diagnostics_before = hax_diag_sequence();
    struct provider *catgw = provider_construct(catgw_factory);
    EXPECT(hax_diag_sequence() == diagnostics_before);
    EXPECT(catgw != NULL);
    if (catgw)
        catgw->destroy(catgw);

    /* The shipped -compatible defs have no default base_url: unavailable
     * (with a pointer at their env alias) and unconstructable until the user
     * supplies one. */
    const struct provider_def *compat = provider_find("openai-compatible");
    EXPECT(compat != NULL);
    EXPECT(selectable("openai-compatible"));
    EXPECT(selectable("anthropic-compatible"));
    struct provider_availability compat_avail = {0};
    provider_prepare_availability(compat, &compat_avail);
    EXPECT(!compat_avail.available);
    EXPECT_STR_EQ(compat_avail.reason, "HAX_OPENAI_BASE_URL not set");
    provider_availability_clear(&compat_avail);
    EXPECT(provider_construct(compat) == NULL);

    /* With a base_url the def is available without probing — the endpoint may serve only
     * its completion route. An api_key (here via its env alias) keys it, and display_name
     * (env alias HAX_OPENAI_DISPLAY_NAME) labels the banner. No catalog identity: an
     * arbitrary endpoint's models are not a hosted vendor's. */
    setenv("HAX_OPENAI_BASE_URL", "http://127.0.0.1:9007/v1/", 1);
    provider_prepare_availability(compat, &compat_avail);
    EXPECT(compat_avail.available);
    EXPECT(compat_avail.url == NULL);
    provider_availability_clear(&compat_avail);

    setenv("HAX_OPENAI_API_KEY", "sk-compat", 1);
    provider_prepare_availability(compat, &compat_avail);
    EXPECT(compat_avail.available);
    EXPECT(compat_avail.url == NULL);

    setenv("HAX_OPENAI_DISPLAY_NAME", "vLLM", 1);
    struct provider *compat_provider = provider_construct(compat);
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
