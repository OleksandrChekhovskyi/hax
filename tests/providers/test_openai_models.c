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
#include "providers/http_provider.h"
#include "providers/registry.h"

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

int main(void)
{
    test_lists_flat_models();
    test_null_data_is_empty_success();
    test_unrecognized_shape_reports_error();
    test_malformed_entry_reports_error();
    T_REPORT();
}
