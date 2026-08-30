/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "diag.h"
#include "effort.h"
#include "harness.h"
#include "provider.h"
#include "util.h"
#include "providers/codex.h"
#include "providers/http_provider.h"
#include "providers/registry.h"

static void test_empty_success_response(void)
{
    char *message = codex_model_catalog_error(200);
    EXPECT_STR_EQ(message, "codex sent an empty or truncated model catalog response");
    free(message);
}

static void test_http_error(void)
{
    char *message = codex_model_catalog_error(503);
    EXPECT_STR_EQ(message, "codex model catalog fetch failed (HTTP 503)");
    free(message);
}

static void test_unreachable(void)
{
    char *message = codex_model_catalog_error(0);
    EXPECT_STR_EQ(message, "could not reach chatgpt.com to list models — check your network");
    free(message);
}

/* Point $HOME at a scratch directory holding a fake codex CLI login, so constructions neither
 * touch a real login nor fail for the lack of one. Returns 0 on success. */
static int setup_scratch_login(void)
{
    char *home = t_tempdir();
    if (!home)
        return -1;

    char path[4096];
    snprintf(path, sizeof(path), "%s/.codex", home);
    if (mkdir(path, 0700) != 0)
        return -1;
    snprintf(path, sizeof(path), "%s/.codex/auth.json", home);
    FILE *auth_file = fopen(path, "w");
    if (!auth_file)
        return -1;
    fputs("{\"tokens\": {\"access_token\": \"t\", \"account_id\": \"a\"}}", auth_file);
    fclose(auth_file);

    setenv("HOME", home, 1);
    /* Keep the developer's own hax credential store out of the auth lookup. */
    unsetenv("XDG_STATE_HOME");
    unsetenv("HAX_MODEL");
    return 0;
}

static void test_display_name_from_own_block(void)
{
    if (setup_scratch_login() != 0)
        T_SKIP("cannot create a scratch codex login");

    struct provider *codex = provider_construct(provider_find("codex"));
    EXPECT(codex != NULL);
    if (codex) {
        EXPECT_STR_EQ(codex->name, "codex");
        /* Costs default to OpenAI-equivalent API rates. */
        EXPECT_STR_EQ(codex->catalog_id, "openai");
        codex->destroy(codex);
    }

    /* The block can rename the catalog identity or opt out of it entirely. */
    config_set_override("providers.codex.catalog_id", "my-rates");
    codex = provider_construct(provider_find("codex"));
    EXPECT(codex != NULL);
    if (codex) {
        EXPECT_STR_EQ(codex->catalog_id, "my-rates");
        codex->destroy(codex);
    }
    config_set_override("providers.codex.catalog_id", "");
    codex = provider_construct(provider_find("codex"));
    EXPECT(codex != NULL);
    if (codex) {
        EXPECT(codex->catalog_id == NULL);
        codex->destroy(codex);
    }
    config_set_override("providers.codex.catalog_id", NULL);

    /* The provider's own block labels the banner; reasoning provenance keeps the stable id. */
    config_set_override("providers.codex.display_name", "Work ChatGPT");
    codex = provider_construct(provider_find("codex"));
    EXPECT(codex != NULL);
    if (codex) {
        EXPECT_STR_EQ(codex->name, "Work ChatGPT");
        EXPECT_STR_EQ(codex->id, "codex");
        codex->destroy(codex);
    }
    config_set_override("providers.codex.display_name", NULL);
}

static int headers_contain(char **headers, const char *needle)
{
    for (char **header = headers; header && *header; header++)
        if (strstr(*header, needle))
            return 1;
    return 0;
}

/* The def wires codex through the generic constructor: its hooks land on the provider, the
 * effort vocabulary replaces the shared ladder, and every request carries the credential and
 * client-identity headers. */
static void test_def_construction_surface(void)
{
    if (setup_scratch_login() != 0)
        T_SKIP("cannot create a scratch codex login");

    /* The def's endpoint-required body members must parse; the pinned endpoint keeps a request
     * capture out of reach, so the shipped literal is validated here. */
    const struct provider_def *def = provider_find("codex");
    json_t *def_extra_body = def->extra_body ? json_loads(def->extra_body, 0, NULL) : NULL;
    EXPECT(json_is_object(json_object_get(def_extra_body, "text")));
    json_decref(def_extra_body);

    struct provider *codex = provider_construct(def);
    EXPECT(codex != NULL);
    if (!codex)
        return;

    EXPECT(codex->list_models == codex_list_models);
    EXPECT(codex->probe_model == codex_probe_model);
    EXPECT(codex->query_usage == codex_query_usage);

    /* The shared ladder is the pre-metadata offer; the codex catalog narrows it per model. */
    const char *const *efforts = NULL;
    EXPECT(codex->list_efforts(codex, &efforts) == EFFORT_LADDER_N);
    EXPECT(efforts == EFFORT_LADDER);

    char **metadata_headers = http_provider_metadata_headers(codex);
    EXPECT(headers_contain(metadata_headers, "Authorization: Bearer t"));
    EXPECT(headers_contain(metadata_headers, "chatgpt-account-id: a"));
    EXPECT(headers_contain(metadata_headers, "originator: codex_cli_rs"));
    EXPECT(headers_contain(metadata_headers, "Accept: application/json"));
    string_array_free(metadata_headers);

    /* Session behavior itself is covered in providers/codex_auth; here only the wiring. */
    EXPECT(http_provider_auth(codex)->ops != NULL);

    codex->destroy(codex);

    /* Credentials come from the auth source, so a static key must warn, not silently vanish. */
    EXPECT(config_load("{\"providers\": {\"codex\": {\"api_key\": \"sk-unused\"}}}") == 0);
    unsigned long diagnostics_before = hax_diag_sequence();
    codex = provider_construct(provider_find("codex"));
    EXPECT(hax_diag_sequence() == diagnostics_before + 1);
    EXPECT(codex != NULL);
    if (codex)
        codex->destroy(codex);
    EXPECT(config_load(NULL) == 0);
}

int main(void)
{
    test_empty_success_response();
    test_http_error();
    test_unreachable();
    test_display_name_from_own_block();
    test_def_construction_surface();
    T_REPORT();
}
