/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "cred_store.h"
#include "harness.h"
#include "provider.h"
#include "xalloc.h"
#include "providers/anthropic_auth.h"
#include "providers/anthropic_login.h"
#include "providers/http_provider.h"
#include "providers/registry.h"

static json_t *store_login(const char *access, const char *login, int lifetime)
{
    json_t *entry =
        json_pack("{s:s,s:s,s:s,s:I}", "access_token", access, "refresh_token", "refresh",
                  "login_id", login, "expires_at", (json_int_t)time(NULL) + lifetime);
    json_object_set_new(entry, "generation", json_integer(0));
    EXPECT(cred_store_set("anthropic", entry) == 0);
    return entry;
}

static void expect_token(const struct http_auth_source *auth, const char *token)
{
    char **headers = auth->ops->headers(auth->state, "session", 1);
    char *expected = xasprintf("Authorization: Bearer %s", token);
    EXPECT_STR_EQ(headers[0], expected);
    EXPECT_STR_EQ(headers[1], "anthropic-version: 2023-06-01");
    EXPECT_STR_EQ(headers[2], "anthropic-beta: oauth-2025-04-20");
    EXPECT_STR_EQ(auth->ops->system_identity(auth->state),
                  "You are Claude Code, Anthropic's official CLI for Claude.");
    string_array_free(headers);
    free(expected);
}

static void test_availability_and_auth_source(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    const struct provider_def *def = provider_find("anthropic");
    EXPECT(def != NULL);
    EXPECT(def->pinned);
    EXPECT_STR_EQ(def->base_url, "https://api.anthropic.com/v1");
    EXPECT_STR_EQ(def->catalog_id, "anthropic");
    struct provider_availability availability = {0};
    provider_prepare_availability(def, &availability);
    EXPECT(!availability.available);
    EXPECT_STR_EQ(availability.reason, "use /login anthropic or set ANTHROPIC_API_KEY");
    provider_availability_clear(&availability);
    struct http_auth_source auth = {0};
    EXPECT(anthropic_auth_source(def, &auth) == -1);
    EXPECT(auth.ops == NULL);
    json_t *entry = store_login("access-1", "login-1", 3600);
    json_decref(entry);
    provider_prepare_availability(def, &availability);
    EXPECT(availability.available);
    EXPECT(availability.url == NULL);
    provider_availability_clear(&availability);
    EXPECT(anthropic_auth_source(def, &auth) == 0);
    EXPECT(auth.ops->prepare(auth.state, 1, NULL, NULL) == 0);
    expect_token(&auth, "access-1");
    auth.ops->destroy(auth.state);
}

static void test_concurrent_refresh_adopted_without_rotation(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    json_t *entry = store_login("expired-access", "login-1", -1);
    json_decref(entry);
    struct http_auth_source auth = {0};
    EXPECT(anthropic_auth_source(NULL, &auth) == 0);
    EXPECT(auth.ops->prepare(auth.state, 0, NULL, NULL) == -1);
    entry = store_login("rotated-access", "login-1", 3600);
    json_object_set_new(entry, "generation", json_integer(1));
    EXPECT(cred_store_set("anthropic", entry) == 0);
    json_decref(entry);
    int retried = 0;
    EXPECT(auth.ops->recover(auth.state, &retried, NULL, NULL) == 1);
    EXPECT(retried == 1);
    expect_token(&auth, "rotated-access");
    EXPECT(auth.ops->recover(auth.state, &retried, NULL, NULL) == 0);
    entry = store_login("another-rotation", "login-1", 7200);
    json_object_set_new(entry, "generation", json_integer(2));
    EXPECT(cred_store_set("anthropic", entry) == 0);
    json_decref(entry);
    EXPECT(auth.ops->prepare(auth.state, 1, NULL, NULL) == 0);
    expect_token(&auth, "another-rotation");
    auth.ops->destroy(auth.state);
}

static void test_rotation_order_is_independent_of_token_lifetime(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    json_t *entry = store_login("old-long-lived", "login-1", 7200);
    json_decref(entry);
    struct http_auth_source auth = {0};
    EXPECT(anthropic_auth_source(NULL, &auth) == 0);
    entry = store_login("new-shorter-lived", "login-1", 3600);
    json_object_set_new(entry, "generation", json_integer(1));
    EXPECT(cred_store_set("anthropic", entry) == 0);
    json_decref(entry);
    EXPECT(auth.ops->prepare(auth.state, 0, NULL, NULL) == 0);
    expect_token(&auth, "new-shorter-lived");
    entry = store_login("stale-disk-token", "login-1", 10000);
    json_decref(entry);
    EXPECT(auth.ops->prepare(auth.state, 0, NULL, NULL) == 0);
    expect_token(&auth, "new-shorter-lived");
    auth.ops->destroy(auth.state);
}

static void test_logout_cannot_resurrect_login(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    json_t *entry = store_login("access", "login-1", 3600);
    json_decref(entry);
    struct http_auth_source auth = {0};
    EXPECT(anthropic_auth_source(NULL, &auth) == 0);
    EXPECT(anthropic_logout_run() == 1);
    EXPECT(auth.ops->prepare(auth.state, 1, NULL, NULL) == -1);
    int retried = 0;
    EXPECT(auth.ops->recover(auth.state, &retried, NULL, NULL) == 0);
    EXPECT(cred_store_get("anthropic") == NULL);
    char *message = auth.ops->unauthorized_message(auth.state);
    EXPECT(strstr(message, "not logged in") != NULL);
    free(message);
    entry = store_login("other-account", "different-login", 7200);
    json_decref(entry);
    EXPECT(auth.ops->prepare(auth.state, 1, NULL, NULL) == -1);
    message = auth.ops->unauthorized_message(auth.state);
    EXPECT(strstr(message, "login changed") != NULL);
    free(message);
    retried = 0;
    EXPECT(auth.ops->recover(auth.state, &retried, NULL, NULL) == 0);
    entry = cred_store_get("anthropic");
    EXPECT_STR_EQ(json_string_value(json_object_get(entry, "access_token")), "other-account");
    json_decref(entry);
    auth.ops->destroy(auth.state);
}

static void test_live_login_logout_and_pinning(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    json_t *entry = store_login("first-token", "first-login", 3600);
    json_decref(entry);
    config_set_override("providers.anthropic.base_url", "http://example.invalid");
    struct provider *provider = http_provider_new(provider_find("anthropic"));
    config_set_override("providers.anthropic.base_url", NULL);
    EXPECT(provider != NULL);
    if (!provider)
        return;
    EXPECT_STR_EQ(http_provider_base_url(provider), "https://api.anthropic.com/v1");
    const struct http_auth_source *auth = http_provider_auth(provider);
    entry = store_login("second-token", "second-login", 3600);
    json_decref(entry);
    EXPECT(auth->ops->prepare(auth->state, 1, NULL, NULL) == -1);
    anthropic_provider_reload_auth(provider);
    EXPECT(auth->ops->prepare(auth->state, 1, NULL, NULL) == 0);
    expect_token(auth, "second-token");
    EXPECT(anthropic_logout_run() == 1);
    anthropic_provider_reload_auth(provider);
    EXPECT(auth->ops->prepare(auth->state, 1, NULL, NULL) == -1);
    provider->destroy(provider);
}

static void test_api_key_fallback_and_live_login(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    setenv("ANTHROPIC_API_KEY", "api-key", 1);
    const struct provider_def *def = provider_find("anthropic");
    struct provider_availability availability = {0};
    provider_prepare_availability(def, &availability);
    EXPECT(availability.available);
    provider_availability_clear(&availability);
    struct provider *provider = provider_construct(def);
    EXPECT(provider != NULL);
    if (!provider)
        return;
    const struct http_auth_source *auth = http_provider_auth(provider);
    EXPECT(auth->ops->prepare(auth->state, 1, NULL, NULL) == 0);
    char **headers = http_provider_metadata_headers(provider);
    EXPECT_STR_EQ(headers[0], "x-api-key: api-key");
    EXPECT_STR_EQ(headers[1], "anthropic-version: 2023-06-01");
    EXPECT(headers[2] == NULL);
    EXPECT(auth->ops->system_identity(auth->state) == NULL);
    string_array_free(headers);
    int retried = 0;
    EXPECT(auth->ops->recover(auth->state, &retried, NULL, NULL) == 0);
    json_t *entry = store_login("oauth-access", "login", 3600);
    json_decref(entry);
    anthropic_provider_reload_auth(provider);
    expect_token(auth, "oauth-access");
    EXPECT(anthropic_logout_run() == 1);
    anthropic_provider_reload_auth(provider);
    EXPECT(auth->ops->prepare(auth->state, 1, NULL, NULL) == 0);
    headers = http_provider_metadata_headers(provider);
    EXPECT_STR_EQ(headers[0], "x-api-key: api-key");
    EXPECT(headers[2] == NULL);
    EXPECT(auth->ops->system_identity(auth->state) == NULL);
    string_array_free(headers);
    provider->destroy(provider);
    unsetenv("ANTHROPIC_API_KEY");
    config_set_override("providers.anthropic.api_key", "inline-key");
    config_set_override("providers.anthropic.version", "2024-01-01");
    provider = provider_construct(def);
    EXPECT(provider != NULL);
    if (provider) {
        headers = http_provider_metadata_headers(provider);
        EXPECT_STR_EQ(headers[0], "x-api-key: inline-key");
        EXPECT_STR_EQ(headers[1], "anthropic-version: 2024-01-01");
        string_array_free(headers);
        provider->destroy(provider);
    }
    config_set_override("providers.anthropic.api_key", NULL);
    config_set_override("providers.anthropic.version", NULL);
}

int main(void)
{
    unsetenv("ANTHROPIC_API_KEY");
    test_availability_and_auth_source();
    test_concurrent_refresh_adopted_without_rotation();
    test_rotation_order_is_independent_of_token_lifetime();
    test_logout_cannot_resurrect_login();
    test_live_login_logout_and_pinning();
    test_api_key_fallback_and_live_login();
    T_REPORT();
}
