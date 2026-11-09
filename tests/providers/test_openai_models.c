/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "harness.h"
#include "loopback.h"
#include "provider.h"
#include "xalloc.h"
#include "providers/http_provider.h"
#include "providers/openai_models.h"
#include "providers/registry.h"
#include "transport/http.h"

static void parse_context_length(const json_t *entry, struct model_info *out)
{
    json_t *context = json_object_get(entry, "context_length");
    if (json_is_integer(context))
        out->context = (long)json_integer_value(context);
}

/* One listing against `response`; returns list_models' rc and hands out its results. */
static int list_from_server(const char *response, struct model_info **models, size_t *n_models,
                            char **error)
{
    struct loopback server = {0};
    loopback_reply_ok(&server, 0, response);
    int port = loopback_start(&server);
    if (port < 0) {
        loopback_stop(&server);
        return -2;
    }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);
    struct provider_def def = {
        .id = "flat",
        .base_url = base_url,
        .parse_model = parse_context_length,
    };
    config_set_override("providers.flat.api_key", "sk-flat");
    struct provider *provider = http_provider_new(&def);
    config_set_override("providers.flat.api_key", NULL);
    EXPECT(provider != NULL);

    int result = -2;
    if (provider) {
        result = provider->list_models(provider, models, n_models, error, NULL, NULL);
        provider->destroy(provider);
    }
    loopback_stop(&server);

    /* The listing authenticates with the OpenAI-side Bearer scheme. */
    if (atomic_load(&server.served) == 1)
        EXPECT(strstr(server.requests[0], "Authorization: Bearer sk-flat\r\n") != NULL);
    return result;
}

/* Usable ids are listed in server order, refined by the def's parse hook; an id-less entry
 * is skipped. */
static void test_lists_flat_models(void)
{
    struct model_info *models = NULL;
    size_t n_models = 0;
    char *error = NULL;
    int result = list_from_server("{\"object\":\"list\",\"data\":["
                                  "{\"id\":\"m1\",\"context_length\":128000},"
                                  "{\"object\":\"model\"},"
                                  "{\"id\":\"m2\"}]}",
                                  &models, &n_models, &error);
    if (result == -2)
        T_SKIP("cannot run a loopback server here");
    EXPECT(result == 0);
    EXPECT(n_models == 2);
    if (n_models == 2) {
        EXPECT_STR_EQ(models[0].id, "m1");
        EXPECT(models[0].context == 128000);
        EXPECT_STR_EQ(models[1].id, "m2");
        EXPECT(models[1].context == 0);
    }
    model_info_free(models, n_models);
    free(error);
}

/* Ollama reports data:null when reachable with no models: an empty success, not an error. */
static void test_null_data_is_empty_success(void)
{
    struct model_info *models = NULL;
    size_t n_models = 1;
    char *error = NULL;
    int result = list_from_server("{\"data\":null}", &models, &n_models, &error);
    if (result == -2)
        T_SKIP("cannot run a loopback server here");
    EXPECT(result == 0);
    EXPECT(n_models == 0);
    EXPECT(models == NULL);
    EXPECT(error == NULL);
}

/* A shape without a model list is a user-reportable error naming the provider. */
static void test_unrecognized_shape_reports_error(void)
{
    struct model_info *models = NULL;
    size_t n_models = 0;
    char *error = NULL;
    int result = list_from_server("{\"models\":[{\"id\":\"m1\"}]}", &models, &n_models, &error);
    if (result == -2)
        T_SKIP("cannot run a loopback server here");
    EXPECT(result == -1);
    EXPECT(models == NULL);
    EXPECT(error != NULL);
    if (error)
        EXPECT(strstr(error, "no model list") != NULL);
    free(error);
}

static void test_malformed_entry_reports_error(void)
{
    struct model_info *models = NULL;
    size_t n_models = 0;
    char *error = NULL;
    int result = list_from_server("{\"data\":[{\"id\":\"m1\"},{\"id\" \"m2\"}]}", &models,
                                  &n_models, &error);
    if (result == -2)
        T_SKIP("cannot run a loopback server here");
    EXPECT(result == -1);
    EXPECT(models == NULL);
    EXPECT(error != NULL);
    if (error)
        EXPECT(strstr(error, "not valid JSON") != NULL);
    free(error);
}

struct test_auth {
    int prepares;
    int refresh_allowed;
    int recoveries;
    int logged_out;
};

static struct test_auth auth_state;

static int test_auth_prepare(void *auth, int allow_refresh, http_tick_cb tick, void *user)
{
    (void)tick;
    (void)user;
    struct test_auth *state = auth;
    state->prepares++;
    state->refresh_allowed = allow_refresh;
    return state->logged_out ? -1 : 0;
}

static char **test_auth_headers(const void *auth, const char *session_id, int streaming)
{
    (void)session_id;
    (void)streaming;
    const struct test_auth *state = auth;
    const char *headers[] = {state->recoveries ? "Authorization: Bearer new-token"
                                               : "Authorization: Bearer old-token",
                             NULL};
    return string_array_concat(headers, NULL);
}

static int test_auth_recover(void *auth, int *retried, http_tick_cb tick, void *user)
{
    (void)tick;
    (void)user;
    if (*retried)
        return 0;
    *retried = 1;
    ((struct test_auth *)auth)->recoveries++;
    return 1;
}

static char *test_auth_message(void *auth)
{
    (void)auth;
    return xstrdup("test login required");
}

static void test_auth_destroy(void *auth)
{
    (void)auth;
}

static int test_auth_source(const struct provider_def *def, struct http_auth_source *out)
{
    (void)def;
    static const struct http_auth_ops ops = {
        .prepare = test_auth_prepare,
        .headers = test_auth_headers,
        .recover = test_auth_recover,
        .unauthorized_message = test_auth_message,
        .destroy = test_auth_destroy,
    };
    out->ops = &ops;
    out->state = &auth_state;
    return 0;
}

static void test_dynamic_credentials_retried_once(void)
{
    static const char unauthorized[] = "HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n"
                                       "Content-Length: 0\r\n\r\n";
    for (int rejected = 0; rejected < 2; rejected++) {
        auth_state = (struct test_auth){0};
        struct loopback server = {.n_requests = 2};
        server.responses[0] = unauthorized;
        if (rejected)
            server.responses[1] = unauthorized;
        else
            loopback_reply_ok(&server, 1, "{\"data\":[{\"id\":\"model\"}]}");
        int port = loopback_start(&server);
        if (port < 0) {
            loopback_stop(&server);
            T_SKIP("cannot run a loopback server here");
        }
        char base_url[64];
        snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);
        struct provider_def def = {
            .id = "dynamic", .base_url = base_url, .auth_source = test_auth_source};
        struct provider *provider = http_provider_new(&def);
        EXPECT(provider != NULL);
        if (provider) {
            struct model_info *models = NULL;
            size_t count = 0;
            char *error = NULL;
            EXPECT(provider->list_models(provider, &models, &count, &error, NULL, NULL) ==
                   (rejected ? -1 : 0));
            EXPECT(auth_state.prepares == 1);
            EXPECT(auth_state.refresh_allowed == 1);
            EXPECT(auth_state.recoveries == 1);
            if (rejected)
                EXPECT_STR_EQ(error, "test login required");
            else
                EXPECT(count == 1);
            model_info_free(models, count);
            free(error);
            provider->destroy(provider);
        }
        loopback_stop(&server);
        EXPECT(atomic_load(&server.served) == 2);
        EXPECT(strstr(server.requests[0], "Authorization: Bearer old-token\r\n") != NULL);
        EXPECT(strstr(server.requests[1], "Authorization: Bearer new-token\r\n") != NULL);
    }
}

static void test_logged_out_metadata_does_not_send(void)
{
    auth_state = (struct test_auth){.logged_out = 1};
    struct provider_def def = {
        .id = "dynamic", .base_url = "http://example.invalid", .auth_source = test_auth_source};
    struct provider *provider = http_provider_new(&def);
    EXPECT(provider != NULL);
    if (!provider)
        return;
    struct model_info *models = NULL;
    size_t count = 0;
    char *error = NULL;
    EXPECT(provider->list_models(provider, &models, &count, &error, NULL, NULL) == -1);
    EXPECT_STR_EQ(error, "test login required");
    EXPECT(auth_state.prepares == 1);
    EXPECT(auth_state.refresh_allowed == 1);
    EXPECT(models == NULL);
    EXPECT(count == 0);
    free(error);
    struct model_probe probe = {0};
    EXPECT(openai_probe_model(provider, "model", &probe) == -1);
    EXPECT(probe.url == NULL);
    EXPECT(auth_state.refresh_allowed == 0);
    auth_state.logged_out = 0;
    EXPECT(openai_probe_model(provider, "model", &probe) == 0);
    EXPECT(probe.url != NULL);
    model_probe_clear(&probe);
    provider->destroy(provider);
}

int main(void)
{
    test_dynamic_credentials_retried_once();
    test_logged_out_metadata_does_not_send();
    test_lists_flat_models();
    test_null_data_is_empty_success();
    test_unrecognized_shape_reports_error();
    test_malformed_entry_reports_error();
    T_REPORT();
}
