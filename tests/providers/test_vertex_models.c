/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "loopback.h"
#include "provider.h"
#include "providers/registry.h"
#include "providers/vertex.h"
#include "providers/vertex_models.h"

/* The shipped def, minus the catalog: a catalog would fetch models.dev behind the test. */
static const struct provider_def VERTEX_DEF = {
    .id = "vertex-ai",
    .display_name = "Vertex AI",
    .api = "openai-completions",
    .api_key_env = "HAX_VERTEX_API_KEY",
    .auth_source = vertex_auth_source,
    .construct = vertex_provider_new,
    .list_models = vertex_list_models,
};

/* The credentials of this machine must not leak into a test about a fake endpoint. */
static void isolate_local_credentials(void)
{
    setenv("HOME", t_tempdir(), 1);
    unsetenv("HAX_VERTEX_API_KEY");
    unsetenv("GOOGLE_APPLICATION_CREDENTIALS");
    unsetenv("VERTEXAI_PROJECT");
    unsetenv("VERTEXAI_LOCATION");
}

/* Point a Vertex provider at `server` and list its models. */
static int list_from(struct loopback *server, struct model_info **models, size_t *n_models,
                     char **error)
{
    int port = loopback_start(server);
    if (port < 0) {
        loopback_stop(server);
        return -2;
    }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);
    setenv("HAX_VERTEX_BASE_URL", base_url, 1);
    setenv("HAX_VERTEX_API_KEY", "test-token", 1);
    setenv("VERTEXAI_PROJECT", "test-project", 1);

    struct provider *provider = provider_construct(&VERTEX_DEF);
    EXPECT(provider != NULL);

    int result = -2;
    if (provider) {
        result = provider->list_models(provider, models, n_models, error, NULL, NULL);
        provider->destroy(provider);
    }
    unsetenv("HAX_VERTEX_BASE_URL");
    unsetenv("HAX_VERTEX_API_KEY");
    unsetenv("VERTEXAI_PROJECT");
    loopback_stop(server);
    return result;
}

/* The listing asks the native publisher API, not the OpenAI-compatible path. */
static void test_lists_chat_models(void)
{
    isolate_local_credentials();
    struct loopback server = {0};
    loopback_reply_ok(&server, 0,
                      "{\"publisherModels\":["
                      "{\"name\":\"publishers/google/models/gemini-3.1-flash-lite\","
                      "\"launchStage\":\"GA\"},"
                      "{\"name\":\"publishers/google/models/gemini-3-flash-preview\","
                      "\"launchStage\":\"PUBLIC_PREVIEW\"}]}");

    struct model_info *models = NULL;
    size_t n_models = 0;
    char *error = NULL;
    int result = list_from(&server, &models, &n_models, &error);
    if (result == -2)
        T_SKIP("cannot run a loopback server here");
    EXPECT(result == 0);
    EXPECT(error == NULL);
    EXPECT(n_models == 2);
    if (n_models == 2) {
        EXPECT_STR_EQ(models[0].id, "gemini-3.1-flash-lite");
        EXPECT(models[0].description == NULL);
        EXPECT_STR_EQ(models[1].id, "gemini-3-flash-preview");
        EXPECT_STR_EQ(models[1].description, "public preview");
    }
    EXPECT(strstr(server.requests[0], "GET /v1beta1/publishers/google/models ") != NULL ||
           strstr(server.requests[0], "GET /v1beta1/publishers/google/models\r\n") != NULL);
    EXPECT(strstr(server.requests[0], "Authorization: Bearer test-token\r\n") != NULL);
    /* The publisher API names no project in its path, so the request must name the quota one. */
    EXPECT(strstr(server.requests[0], "x-goog-user-project: test-project\r\n") != NULL);
    model_info_free(models, n_models);
    free(error);
}

/* A second page arrives when the first one names its successor. */
static void test_follows_the_next_page(void)
{
    isolate_local_credentials();
    struct loopback server = {0};
    loopback_reply_ok(&server, 0,
                      "{\"publisherModels\":["
                      "{\"name\":\"publishers/google/models/gemini-3.1-flash-lite\"}],"
                      "\"nextPageToken\":\"page-2\"}");
    loopback_reply_ok(&server, 1,
                      "{\"publisherModels\":["
                      "{\"name\":\"publishers/google/models/gemini-3.8-flash\"}]}");
    server.n_requests = 2;

    struct model_info *models = NULL;
    size_t n_models = 0;
    char *error = NULL;
    int result = list_from(&server, &models, &n_models, &error);
    if (result == -2)
        T_SKIP("cannot run a loopback server here");
    EXPECT(result == 0);
    EXPECT(n_models == 2);
    if (n_models == 2) {
        EXPECT_STR_EQ(models[0].id, "gemini-3.1-flash-lite");
        EXPECT_STR_EQ(models[1].id, "gemini-3.8-flash");
    }
    EXPECT(strstr(server.requests[1], "pageToken=page-2") != NULL);
    model_info_free(models, n_models);
    free(error);
}

/* Model Garden lists families the chat surface cannot serve, and previews few projects reach. */
static void test_skips_models_the_chat_surface_cannot_serve(void)
{
    struct model_info *models = NULL;
    size_t n_models = 0;
    char *next_page_token = NULL;
    char *error = NULL;
    int result = vertex_parse_publisher_models(
        "{\"publisherModels\":["
        "{\"name\":\"publishers/google/models/gemini-3.1-flash-lite\",\"launchStage\":\"GA\"},"
        "{\"name\":\"publishers/google/models/gemini-embedding-001\",\"launchStage\":\"GA\"},"
        "{\"name\":\"publishers/google/models/"
        "gemini-2.5-flash-preview-tts\",\"launchStage\":\"GA\"},"
        "{\"name\":\"publishers/google/models/gemini-2.0-flash-live-001\",\"launchStage\":\"GA\"},"
        "{\"name\":\"publishers/google/models/gemini-3.5-transcribe\",\"launchStage\":\"GA\"},"
        "{\"name\":\"publishers/google/models/gemma-3-27b-it\",\"launchStage\":\"GA\"},"
        "{\"name\":\"publishers/google/models/"
        "gemini-4-experimental\",\"launchStage\":\"EXPERIMENTAL\"},"
        "{\"name\":\"publishers/anthropic/models/claude-opus-4-5\"}]}",
        &models, &n_models, &next_page_token, &error);
    EXPECT(result == 0);
    EXPECT(error == NULL);
    EXPECT(n_models == 1);
    if (n_models == 1)
        EXPECT_STR_EQ(models[0].id, "gemini-3.1-flash-lite");
    EXPECT(next_page_token == NULL);
    model_info_free(models, n_models);
    free(next_page_token);
    free(error);
}

/* A page without a model list is a user-reportable error. */
static void test_reports_a_page_without_models(void)
{
    struct model_info *models = NULL;
    size_t n_models = 0;
    char *next_page_token = NULL;
    char *error = NULL;
    int result = vertex_parse_publisher_models("{\"publisherModels\":{}}", &models, &n_models,
                                               &next_page_token, &error);
    EXPECT(result == -1);
    EXPECT(models == NULL);
    EXPECT(error != NULL);
    if (error)
        EXPECT(strstr(error, "no model list") != NULL);
    free(next_page_token);
    free(error);
}

/* A body that is not JSON is a user-reportable error. */
static void test_reports_a_malformed_body(void)
{
    struct model_info *models = NULL;
    size_t n_models = 0;
    char *next_page_token = NULL;
    char *error = NULL;
    int result = vertex_parse_publisher_models("<html>not json</html>", &models, &n_models,
                                               &next_page_token, &error);
    EXPECT(result == -1);
    EXPECT(models == NULL);
    EXPECT(error != NULL);
    if (error)
        EXPECT(strstr(error, "not valid JSON") != NULL);
    free(next_page_token);
    free(error);
}

/* An empty catalog is a successful listing of nothing. */
static void test_reports_an_empty_catalog(void)
{
    struct model_info *models = NULL;
    size_t n_models = 0;
    char *next_page_token = NULL;
    char *error = NULL;
    int result = vertex_parse_publisher_models("{\"publisherModels\":[]}", &models, &n_models,
                                               &next_page_token, &error);
    EXPECT(result == 0);
    EXPECT(n_models == 0);
    EXPECT(models == NULL);
    EXPECT(error == NULL);
    free(next_page_token);
    free(error);
}

/* A rejected credential names the credentials, not the endpoint. */
static void test_reports_a_rejected_credential(void)
{
    isolate_local_credentials();
    struct loopback server = {0};
    server.responses[0] =
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

    struct model_info *models = NULL;
    size_t n_models = 0;
    char *error = NULL;
    int result = list_from(&server, &models, &n_models, &error);
    if (result == -2)
        T_SKIP("cannot run a loopback server here");
    EXPECT(result == -1);
    EXPECT(models == NULL);
    EXPECT(error != NULL);
    if (error)
        EXPECT(strstr(error, "Vertex AI authentication failed") != NULL);
    model_info_free(models, n_models);
    free(error);
}

int main(void)
{
    test_lists_chat_models();
    test_follows_the_next_page();
    test_skips_models_the_chat_surface_cannot_serve();
    test_reports_a_page_without_models();
    test_reports_a_malformed_body();
    test_reports_an_empty_catalog();
    test_reports_a_rejected_credential();
    T_REPORT();
}
