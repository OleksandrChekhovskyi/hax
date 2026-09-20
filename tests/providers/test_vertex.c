/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "catalog.h"
#include "config.h"
#include "diag.h"
#include "harness.h"
#include "loopback.h"
#include "provider.h"
#include "xalloc.h"
#include "providers/anthropic_body.h"
#include "providers/http_provider.h"
#include "providers/registry.h"
#include "providers/vertex.h"
#include "providers/wire.h"
#include "system/fs.h"

/* Endpoint host follows the resolved location: `global`, a `us`/`eu` multi-region, or the
 * regional host. An explicit base_url would win verbatim instead. */
static void test_resolve_base_url_host_rules(void)
{
    EXPECT(config_load("{\"providers\": {\"vertex\": {\"project\": \"proj-1\","
                       " \"location\": \"global\"}}}") == 0);
    char *url = vertex_resolve_base_url(NULL);
    EXPECT_STR_EQ(url, "https://aiplatform.googleapis.com");
    free(url);

    EXPECT(config_load("{\"providers\": {\"vertex\": {\"project\": \"proj-1\","
                       " \"location\": \"us\"}}}") == 0);
    url = vertex_resolve_base_url(NULL);
    EXPECT_STR_EQ(url, "https://aiplatform.us.rep.googleapis.com");
    free(url);

    EXPECT(config_load("{\"providers\": {\"vertex\": {\"project\": \"proj-1\","
                       " \"location\": \"eu\"}}}") == 0);
    url = vertex_resolve_base_url(NULL);
    EXPECT_STR_EQ(url, "https://aiplatform.eu.rep.googleapis.com");
    free(url);

    EXPECT(config_load("{\"providers\": {\"vertex\": {\"project\": \"proj-1\","
                       " \"location\": \"us-central1\"}}}") == 0);
    url = vertex_resolve_base_url(NULL);
    EXPECT_STR_EQ(url, "https://us-central1-aiplatform.googleapis.com");
    free(url);

    /* The default location fills in when none is set. */
    EXPECT(config_load("{\"providers\": {\"vertex\": {\"project\": \"proj-1\"}}}") == 0);
    url = vertex_resolve_base_url(NULL);
    EXPECT_STR_EQ(url, "https://us-east5-aiplatform.googleapis.com");
    free(url);

    /* Missing project fails with a diagnostic, so availability and construction report it. */
    EXPECT(config_load("{ }") == 0);
    unsigned long diagnostics_before = hax_diag_sequence();
    url = vertex_resolve_base_url(NULL);
    EXPECT(url == NULL);
    EXPECT(hax_diag_sequence() == diagnostics_before + 1);
    EXPECT(config_load(NULL) == 0);
}

static int idx_of(const char *name)
{
    size_t n;
    const struct provider_def *const *all = provider_all(&n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(all[i]->id, name) == 0)
            return (int)i;
    return -1;
}

/* vertex is a data def in autoselect order, below the gateways and above the local servers. */
static void test_def_registered(void)
{
    const struct provider_def *def = provider_find("vertex");
    EXPECT(def != NULL);
    if (!def)
        return;
    EXPECT_STR_EQ(def->display_name, "Vertex AI");
    EXPECT_STR_EQ(def->api, "anthropic-messages");
    EXPECT_STR_EQ(def->catalog_id, "google-vertex-anthropic");
    EXPECT_STR_EQ(def->version, "vertex-2023-10-16");
    EXPECT(def->body_version == 1);
    EXPECT(def->strict_signatures == 1);
    EXPECT(def->auth_source == vertex_auth_source);
    EXPECT(def->resolve_base_url == vertex_resolve_base_url);
    EXPECT(def->list_models == http_provider_list_catalog_models);
    EXPECT(strstr(def->path_template, "{model}") != NULL);
    EXPECT(strstr(def->path_template, "{project}") != NULL);
    EXPECT(strstr(def->path_template, "{location}") != NULL);

    EXPECT(idx_of("vertex") > idx_of("opencode-go"));
    EXPECT(idx_of("vertex") < idx_of("llamacpp"));
    EXPECT(provider_default() == provider_find("codex"));
}

/* The bare Messages wire carries the model and the version header; the Vertex raw-Predict
 * variant drops the model member and puts anthropic_version in the body. */
static void test_messages_body_variant(void)
{
    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = "hello"}};
    struct context context = {.items = items, .n_items = 1, .image_input = 1};
    struct wire_body_opts opts = {
        .max_tokens = 32000, .anthropic_version = "vertex-2023-10-16", .omit_model = 1};
    json_t *body = anthropic_build_body(&context, "vertex", "claude-x", &opts);
    EXPECT(json_object_get(body, "model") == NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(body, "anthropic_version")),
                  "vertex-2023-10-16");
    json_decref(body);

    struct wire_body_opts default_opts = {.max_tokens = 32000};
    body = anthropic_build_body(&context, "vertex", "claude-x", &default_opts);
    EXPECT_STR_EQ(json_string_value(json_object_get(body, "model")), "claude-x");
    EXPECT(json_object_get(body, "anthropic_version") == NULL);
    json_decref(body);
}

struct error_log {
    int n_errors;
    char message[256];
};

static int log_error(const struct stream_event *event, void *user)
{
    struct error_log *log = user;
    if (event->kind == EV_ERROR) {
        log->n_errors++;
        snprintf(log->message, sizeof(log->message), "%s", event->u.error.message);
    }
    return 0;
}

/* End-to-end: an explicit base_url wins verbatim, the path template expands project/location
 * now and the model per request, auth is a Google bearer, and the body is the raw-Predict shape
 * (no model member, anthropic_version in the body, no anthropic-version header). */
static void test_stream_raw_predict(void)
{
    struct loopback server = {
        .response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 2\r\nConnection: close\r\n\r\nno",
        .n_requests = 1,
    };
    int port = loopback_listen(&server);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);
    char *config_json = xasprintf("{\"providers\": {\"vertex\": {\"base_url\": \"%s\","
                                  " \"project\": \"proj-1\", \"location\": \"us-east5\","
                                  " \"access_token\": \"gcp-tok\"}}}",
                                  base_url);
    EXPECT(config_load(config_json) == 0);
    free(config_json);
    const struct provider_def *def = provider_find("vertex");
    struct provider *provider = provider_construct(def);
    EXPECT(provider != NULL);
    if (!provider)
        goto out_server;
    int rc = loopback_serve(&server);
    EXPECT(rc == 0);
    if (rc != 0)
        goto out_provider;

    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = "hello"}};
    struct context context = {.items = items, .n_items = 1, .image_input = 1};
    struct error_log log = {0};
    const char *model = "claude-sonnet@20250929";
    provider->stream(provider, &context, model, log_error, &log, NULL, NULL);
    loopback_stop(&server);
    EXPECT(atomic_load(&server.served) == 1);
    EXPECT(log.n_errors == 1);
    EXPECT(strstr(server.requests[0], "POST /v1/projects/proj-1/locations/us-east5/"
                                      "publishers/anthropic/models/claude-sonnet@20250929:"
                                      "streamRawPredict HTTP") != NULL);
    EXPECT(strstr(server.requests[0], "Authorization: Bearer gcp-tok\r\n") != NULL);
    /* Vertex reads the version from the body, not the anthropic-version header. */
    EXPECT(strstr(server.requests[0], "anthropic-version:") == NULL);
    EXPECT(strstr(server.requests[0], "\"anthropic_version\":\"vertex-2023-10-16\"") != NULL);
    EXPECT(strstr(server.requests[0], "\"model\":") == NULL);
    EXPECT(strstr(server.requests[0], "\"max_tokens\":8192") != NULL);

out_provider:
    provider->destroy(provider);
out_server:
    loopback_stop(&server);
    EXPECT(config_load(NULL) == 0);
}

/* Write a fresh authorized_user ADC file into a temp dir and hand the path out via
 * GOOGLE_APPLICATION_CREDENTIALS. */
static void write_adc_user_file(const char *refresh_token)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/adc.json", dir);
    char *contents = xasprintf("{\"type\":\"authorized_user\",\"client_id\":\"cid\","
                               "\"client_secret\":\"csecret\",\"refresh_token\":\"%s\"}",
                               refresh_token);
    EXPECT(fs_write_atomic(path, contents, strlen(contents), 0) == 0);
    free(contents);
    setenv("GOOGLE_APPLICATION_CREDENTIALS", path, 1);
    free(path);
}

/* Prompt-less auth through an authorized_user ADC file: a 401 to one crafted to expiring
 * credentials recovers by refreshing against the (overridable) token endpoint, and the rebuilt
 * request carries the new token. */
static void test_auth_source_user_refresh(void)
{
    struct loopback oauth = {.n_requests = 2};
    loopback_reply_ok(&oauth, 0, "{\"access_token\": \"fresh-2\", \"expires_in\": 3600}");
    oauth.response = oauth.responses[0];
    char oauth_url[64];
    int oauth_port = loopback_start(&oauth);
    EXPECT(oauth_port > 0);
    if (oauth_port <= 0) {
        loopback_stop(&oauth);
        return;
    }
    snprintf(oauth_url, sizeof(oauth_url), "http://127.0.0.1:%d", oauth_port);
    setenv("HAX_VERTEX_OAUTH_URL", oauth_url, 1);

    write_adc_user_file("old-refresh");

    struct http_auth_source source = {0};
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    /* First call has no token: prepare runs the refresh exchange. */
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == 0);
    char **headers = source.ops->headers(source.state, "sid", 1);
    EXPECT(strstr(headers[0], "Authorization: Bearer fresh-2") != NULL);
    string_array_free(headers);

    /* A logged-out/rejected state recovers by forcing one refresh against the endpoint. */
    int retried = 0;
    EXPECT(source.ops->recover(source.state, &retried, NULL, NULL) == 1);
    EXPECT(retried == 1);
    char *message = source.ops->unauthorized_message(source.state);
    EXPECT(strstr(message, "gcloud auth application-default login") != NULL);
    free(message);

    loopback_stop(&oauth);
    EXPECT(atomic_load(&oauth.served) == 2);
    /* The refresh is a form POST carrying the ADC's own refresh token. */
    for (int i = 0; i < 2; i++) {
        EXPECT(strstr(oauth.requests[i], "grant_type=refresh_token") != NULL);
        EXPECT(strstr(oauth.requests[i], "refresh_token=old-refresh") != NULL);
    }

    source.ops->destroy(source.state);
    unsetenv("HAX_VERTEX_OAUTH_URL");
    unsetenv("GOOGLE_APPLICATION_CREDENTIALS");
}

/* A missing credential source never blocks construction: the session reports the resolution
 * step on the first request and on availability. */
static void test_auth_source_literal(void)
{
    EXPECT(config_load("{\"providers\": {\"vertex\": {\"project\": \"proj\","
                       " \"access_token\": \"lit-tok\"}}}") == 0);
    struct http_auth_source source = {0};
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == 0);
    char **headers = source.ops->headers(source.state, "sid", 1);
    EXPECT(strstr(headers[0], "Authorization: Bearer lit-tok") != NULL);
    string_array_free(headers);
    source.ops->destroy(source.state);

    /* With the explicit token configured, availability passes even without an ADC file. */
    struct provider_availability availability = {0};
    vertex_prepare_availability(provider_find("vertex"), &availability);
    EXPECT(availability.available);
    provider_availability_clear(&availability);
    EXPECT(config_load(NULL) == 0);
}

static void setup_fixtures(void)
{
    char *home = t_tempdir();
    setenv("HOME", home, 1);
    setenv("XDG_CONFIG_HOME", home, 1);
    setenv("XDG_CACHE_HOME", home, 1);
    setenv("CLOUDSDK_CONFIG", home, 1);
    unsetenv("GOOGLE_CLOUD_PROJECT");
    unsetenv("ANTHROPIC_VERTEX_PROJECT_ID");
    unsetenv("GOOGLE_CLOUD_LOCATION");
    unsetenv("CLOUD_ML_REGION");
    unsetenv("GOOGLE_OAUTH_ACCESS_TOKEN");
    unsetenv("GOOGLE_APPLICATION_CREDENTIALS");
    unsetenv("HAX_VERTEX_OAUTH_URL");
    setenv("NO_PROXY", "127.0.0.1,localhost", 1);
    setenv("no_proxy", "127.0.0.1,localhost", 1);
    config_set_override("catalog.refresh", "0");

    char *path = xasprintf("%s/hax/catalog.json", home);
    static const char catalog[] = "{\"google-vertex-anthropic\":{\"models\":{"
                                  "\"claude-sonnet@20250929\":{\"limit\":{\"output\":8192}}}}}";
    EXPECT(fs_write_atomic(path, catalog, sizeof(catalog) - 1, 0) == 0);
    free(path);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    EXPECT(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    setup_fixtures();
    test_resolve_base_url_host_rules();
    test_def_registered();
    test_messages_body_variant();
    test_stream_raw_predict();
    test_auth_source_user_refresh();
    test_auth_source_literal();
    catalog_shutdown();
    config_free();
    curl_global_cleanup();
    T_REPORT();
}
