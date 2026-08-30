/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "diag.h"
#include "harness.h"
#include "provider.h"
#include "providers/http_provider.h"
#include "providers/llamacpp.h"
#include "providers/registry.h"

static const char MODELS_RESPONSE[] =
    "{\"data\": [{\"id\": 7}, {}, {\"id\": \"served-a\"}, {\"id\": \"served-b\"}]}";

static const char ROUTER_RESPONSE[] =
    "{\"data\": ["
    "{\"id\": \"idle-a\", \"status\": {\"value\": \"unloaded\"}},"
    "{\"id\": \"running\", \"aliases\": [\"short-name\"], \"status\": {\"value\": \"loaded\"}},"
    "{\"id\": \"idle-b\", \"status\": {\"value\": \"unloaded\"}}]}";

static const char ROUTER_IDLE_RESPONSE[] =
    "{\"data\": ["
    "{\"id\": \"idle-a\", \"status\": {\"value\": \"unloaded\"}},"
    "{\"id\": \"idle-b\", \"status\": {\"value\": \"unloaded\"}}]}";

static const char ROUTER_BUSY_RESPONSE[] =
    "{\"data\": ["
    "{\"id\": \"running-a\", \"status\": {\"value\": \"loaded\"}},"
    "{\"id\": \"running-b\", \"status\": {\"value\": \"sleeping\"}}]}";

static const char ROUTER_LOADING_RESPONSE[] =
    "{\"data\": ["
    "{\"id\": \"idle-a\", \"status\": {\"value\": \"unloaded\"}},"
    "{\"id\": \"warming\", \"status\": {\"value\": \"loading\"}}]}";

static const char ROUTER_LOADED_AND_LOADING_RESPONSE[] =
    "{\"data\": ["
    "{\"id\": \"running\", \"status\": {\"value\": \"loaded\"}},"
    "{\"id\": \"warming\", \"status\": {\"value\": \"loading\"}}]}";

static void expect_label(const char *model, const char *expected)
{
    char *label = llamacpp_model_label(NULL, model);
    EXPECT_STR_EQ(label, expected);
    free(label);
}

static void test_model_label(void)
{
    expect_label("/home/user/models/Qwen3.6-35B-A3B-UD-Q5_K_XL.gguf", "Qwen3.6-35B-A3B-UD-Q5_K_XL");
    expect_label("C:\\models\\Qwen.GGUF", "Qwen");
    expect_label("Qwen.gguf", "Qwen");
    expect_label("owner/model", "owner/model");
    expect_label("/models/model.bin", "/models/model.bin");
    expect_label(".gguf", ".gguf");
}

static void test_model_warning(void)
{
    char *warning = llamacpp_model_warning("codex/gpt-5.6-sol/high",
                                           "/home/user/models/Qwen3.6-35B-A3B-UD-Q5_K_XL.gguf");
    EXPECT_STR_EQ(warning, "llama.cpp: model 'codex/gpt-5.6-sol/high' is not served — using "
                           "'Qwen3.6-35B-A3B-UD-Q5_K_XL'");
    free(warning);

    warning = llamacpp_model_warning("/old/Qwen.gguf", "/new/Qwen.gguf");
    EXPECT_STR_EQ(warning, "llama.cpp: configured model is not served — using 'Qwen'");
    free(warning);
}

static struct llamacpp_reconcile reconcile(const char *body, const char *configured_model)
{
    struct llamacpp_reconcile decision;
    EXPECT(llamacpp_reconcile_model(body, configured_model, &decision) == 0);
    return decision;
}

static void test_reconcile_unconfigured_model(void)
{
    struct llamacpp_reconcile decision = reconcile(MODELS_RESPONSE, NULL);
    EXPECT_STR_EQ(decision.replacement, "served-a");
    free(decision.replacement);

    decision = reconcile(MODELS_RESPONSE, "");
    EXPECT_STR_EQ(decision.replacement, "served-a");
    free(decision.replacement);
}

static void test_reconcile_served_model(void)
{
    struct llamacpp_reconcile decision = reconcile(MODELS_RESPONSE, "served-b");
    EXPECT(decision.replacement == NULL);
    EXPECT(!decision.clear_configured);
}

static void test_reconcile_unserved_model(void)
{
    struct llamacpp_reconcile decision = reconcile(MODELS_RESPONSE, "stale-model");
    EXPECT_STR_EQ(decision.replacement, "served-a");
    EXPECT(!decision.clear_configured);
    free(decision.replacement);
}

static void test_reconcile_router_unconfigured(void)
{
    struct llamacpp_reconcile decision = reconcile(ROUTER_RESPONSE, NULL);
    EXPECT_STR_EQ(decision.replacement, "running");
    free(decision.replacement);

    decision = reconcile(ROUTER_IDLE_RESPONSE, NULL);
    EXPECT(decision.replacement == NULL);
    EXPECT(!decision.no_models);

    decision = reconcile(ROUTER_BUSY_RESPONSE, NULL);
    EXPECT(decision.replacement == NULL);

    decision = reconcile(ROUTER_LOADING_RESPONSE, NULL);
    EXPECT_STR_EQ(decision.replacement, "warming");
    free(decision.replacement);

    decision = reconcile(ROUTER_LOADED_AND_LOADING_RESPONSE, NULL);
    EXPECT(decision.replacement == NULL);
}

static void test_reconcile_router_configured(void)
{
    struct llamacpp_reconcile decision = reconcile(ROUTER_RESPONSE, "idle-b");
    EXPECT(decision.replacement == NULL);
    EXPECT(decision.canonical == NULL);
    EXPECT(!decision.clear_configured);

    /* A configured alias is kept but normalized to its catalog id. */
    decision = reconcile(ROUTER_RESPONSE, "short-name");
    EXPECT(decision.replacement == NULL);
    EXPECT_STR_EQ(decision.canonical, "running");
    EXPECT(!decision.clear_configured);
    free(decision.canonical);

    decision = reconcile(ROUTER_RESPONSE, "stale-model");
    EXPECT(decision.replacement == NULL);
    EXPECT(decision.clear_configured);
}

static void test_reconcile_empty_catalog(void)
{
    struct llamacpp_reconcile decision = reconcile("{\"data\": []}", "model");
    EXPECT(decision.no_models);
    EXPECT(decision.replacement == NULL);
    EXPECT(!decision.clear_configured);
}

static void test_reconcile_unusable_response(void)
{
    struct llamacpp_reconcile decision;
    EXPECT(llamacpp_reconcile_model("not json", "model", &decision) == -1);
    EXPECT(llamacpp_reconcile_model("{\"data\": \"nope\"}", "model", &decision) == -1);
}

static void test_parse_model(void)
{
    json_t *entry = json_loads("{\"id\": \"running\", \"status\": {\"value\": \"loaded\"}, "
                               "\"architecture\": {\"input_modalities\": [\"text\", \"image\"]}, "
                               "\"meta\": {\"n_ctx\": 32768}}",
                               0, NULL);
    struct model_info info;
    model_info_init(&info);
    llamacpp_parse_model(entry, &info);
    EXPECT(info.context == 32768);
    EXPECT(info.image_input == PROVIDER_CAP_YES);
    EXPECT_STR_EQ(info.description, "loaded");
    free(info.description);
    json_decref(entry);
}

static void test_parse_model_idle_text_only(void)
{
    json_t *entry = json_loads("{\"id\": \"idle\", \"status\": {\"value\": \"unloaded\"}, "
                               "\"architecture\": {\"input_modalities\": [\"text\"]}}",
                               0, NULL);
    struct model_info info;
    model_info_init(&info);
    llamacpp_parse_model(entry, &info);
    EXPECT(info.context == 0);
    EXPECT(info.image_input == PROVIDER_CAP_NO);
    EXPECT(info.description == NULL);
    json_decref(entry);
}

static void test_parse_model_failed(void)
{
    json_t *entry = json_loads("{\"id\": \"broken\", \"status\": {\"value\": \"unloaded\", "
                               "\"failed\": true, \"exit_code\": 137}}",
                               0, NULL);
    struct model_info info;
    model_info_init(&info);
    llamacpp_parse_model(entry, &info);
    EXPECT_STR_EQ(info.description, "failed (exit 137)");
    free(info.description);
    json_decref(entry);
}

static void test_parse_model_single_mode(void)
{
    json_t *entry = json_loads("{\"id\": \"model.gguf\", \"meta\": {\"n_ctx\": 4096}}", 0, NULL);
    struct model_info info;
    model_info_init(&info);
    llamacpp_parse_model(entry, &info);
    EXPECT(info.context == 4096);
    EXPECT(info.image_input == PROVIDER_CAP_UNKNOWN);
    EXPECT(info.description == NULL);
    json_decref(entry);
}

static void test_unscoped_props_url(void)
{
    char *url = llamacpp_props_url("http://127.0.0.1:18080/v1", NULL);
    EXPECT_STR_EQ(url, "http://127.0.0.1:18080/props");
    free(url);

    url = llamacpp_props_url("http://127.0.0.1:18080/v1", "");
    EXPECT_STR_EQ(url, "http://127.0.0.1:18080/props");
    free(url);
}

static void test_model_scoped_props_url(void)
{
    char *url = llamacpp_props_url("http://127.0.0.1:18080/v1", "/models/Qwen 3.gguf");
    /* libcurl versions differ in the case of percent-escape hex digits. */
    EXPECT(strcmp(url, "http://127.0.0.1:18080/props?model=%2Fmodels%2FQwen+3.gguf") == 0 ||
           strcmp(url, "http://127.0.0.1:18080/props?model=%2fmodels%2fQwen+3.gguf") == 0);
    free(url);
}

/* The def's endpoint derives from the registered port setting unless base_url overrides it. */
static void test_default_base_url_follows_port(void)
{
    config_set_override("providers.llamacpp.port", "1");
    config_set_override("model", "local.gguf"); /* keeps the unreachable server non-fatal */

    struct provider *provider = provider_construct(provider_find("llamacpp"));
    EXPECT(provider != NULL);
    if (provider) {
        EXPECT_STR_EQ(http_provider_base_url(provider), "http://127.0.0.1:1/v1");
        provider->destroy(provider);
    }

    config_set_override("model", NULL);
    config_set_override("providers.llamacpp.port", NULL);
}

/* The def constructs through the generic path: discovery keeps an explicitly configured model
 * while the server is unreachable, the hooks land on the provider, and without a configured
 * model an unreachable server fails construction with guidance. */
static void test_def_construction(void)
{
    config_set_override("providers.llamacpp.base_url", "http://127.0.0.1:1/v1");
    config_set_override("model", "local.gguf");

    /* The def's endpoint-required body members must parse (return_progress rides extra_body). */
    const struct provider_def *def = provider_find("llamacpp");
    json_t *def_extra_body = def->extra_body ? json_loads(def->extra_body, 0, NULL) : NULL;
    EXPECT(json_is_true(json_object_get(def_extra_body, "return_progress")));
    json_decref(def_extra_body);

    struct provider *provider = provider_construct(def);
    EXPECT(provider != NULL);
    if (provider) {
        EXPECT_STR_EQ(provider->name, "llama.cpp");
        EXPECT_STR_EQ(provider->id, "llamacpp");
        EXPECT(provider->probe_model == llamacpp_probe_model);
        EXPECT(provider->model_label == llamacpp_model_label);
        EXPECT(!provider->model_discovered);
        const char *const *efforts = NULL;
        EXPECT(provider->list_efforts(provider, &efforts) == 0);
        provider->destroy(provider);
    }

    config_set_override("model", NULL);
    unsetenv("HAX_MODEL");
    unsigned long diagnostics_before = hax_diag_sequence();
    EXPECT(provider_construct(provider_find("llamacpp")) == NULL);
    EXPECT(hax_diag_sequence() == diagnostics_before + 1);

    config_set_override("providers.llamacpp.base_url", NULL);
}

int main(void)
{
    test_model_label();
    test_model_warning();
    test_reconcile_unconfigured_model();
    test_reconcile_served_model();
    test_reconcile_unserved_model();
    test_reconcile_router_unconfigured();
    test_reconcile_router_configured();
    test_reconcile_empty_catalog();
    test_reconcile_unusable_response();
    test_parse_model();
    test_parse_model_idle_text_only();
    test_parse_model_failed();
    test_parse_model_single_mode();
    test_unscoped_props_url();
    test_model_scoped_props_url();
    test_default_base_url_follows_port();
    test_def_construction();
    T_REPORT();
}
