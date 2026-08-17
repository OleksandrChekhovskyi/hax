/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "config.h"
#include "harness.h"
#include "provider.h"
#include "providers/codex.h"

static void test_token_expired(void)
{
    char *message = codex_model_catalog_error(401);
    EXPECT_STR_EQ(message, "codex token expired — run `codex` once to refresh, then retry");
    free(message);
}

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

/* Constructs against a scratch $HOME so no real codex login is touched. */
static void test_display_name_from_own_block(void)
{
    char *home = t_tempdir();
    if (!home)
        T_SKIP("cannot create a scratch home");

    char path[4096];
    snprintf(path, sizeof(path), "%s/.codex", home);
    if (mkdir(path, 0700) != 0)
        T_SKIP("cannot create a scratch ~/.codex");
    snprintf(path, sizeof(path), "%s/.codex/auth.json", home);
    FILE *auth_file = fopen(path, "w");
    if (!auth_file)
        T_SKIP("cannot write a scratch auth.json");
    fputs("{\"tokens\": {\"access_token\": \"t\", \"account_id\": \"a\"}}", auth_file);
    fclose(auth_file);

    setenv("HOME", home, 1);
    unsetenv("HAX_MODEL");

    struct provider *codex = codex_provider_new("codex");
    EXPECT(codex != NULL);
    if (codex) {
        EXPECT_STR_EQ(codex->name, "codex");
        codex->destroy(codex);
    }

    /* The provider's own block labels the banner; reasoning provenance keeps the stable id. */
    config_set_override("providers.codex.display_name", "Work ChatGPT");
    codex = codex_provider_new("codex");
    EXPECT(codex != NULL);
    if (codex) {
        EXPECT_STR_EQ(codex->name, "Work ChatGPT");
        EXPECT_STR_EQ(codex->id, "codex");
        codex->destroy(codex);
    }
    config_set_override("providers.codex.display_name", NULL);
}

int main(void)
{
    test_token_expired();
    test_empty_success_response();
    test_http_error();
    test_unreachable();
    test_display_name_from_own_block();
    T_REPORT();
}
