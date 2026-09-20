/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <sys/stat.h>

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

static void expect_resolved_endpoint(const char *host, const char *project, const char *location)
{
    char *url = vertex_resolve_base_url(NULL);
    EXPECT_STR_EQ(url, host);
    free(url);
    char *path = vertex_resolve_path(NULL);
    char *want = xasprintf("/v1/projects/%s/locations/%s/publishers/anthropic/models/"
                           "{model}:streamRawPredict",
                           project, location);
    EXPECT_STR_EQ(path, want);
    free(want);
    free(path);
}

static void clear_vertex_setting_env(void)
{
    unsetenv("HAX_VERTEX_PROJECT");
    unsetenv("HAX_VERTEX_LOCATION");
    unsetenv("GOOGLE_CLOUD_PROJECT");
    unsetenv("ANTHROPIC_VERTEX_PROJECT_ID");
    unsetenv("GOOGLE_CLOUD_LOCATION");
    unsetenv("CLOUD_ML_REGION");
}

static void test_endpoint_setting_precedence(void)
{
    clear_vertex_setting_env();
    EXPECT(config_load("{\"providers\": {\"vertex\": {\"project\": \"proj-1\","
                       " \"location\": \"global\"}}}") == 0);
    expect_resolved_endpoint("https://aiplatform.googleapis.com", "proj-1", "global");

    EXPECT(config_load("{\"providers\": {\"vertex\": {\"project\": \"proj-1\","
                       " \"location\": \"us\"}}}") == 0);
    expect_resolved_endpoint("https://aiplatform.us.rep.googleapis.com", "proj-1", "us");

    EXPECT(config_load("{\"providers\": {\"vertex\": {\"project\": \"proj-1\","
                       " \"location\": \"eu\"}}}") == 0);
    expect_resolved_endpoint("https://aiplatform.eu.rep.googleapis.com", "proj-1", "eu");

    EXPECT(config_load("{\"providers\": {\"vertex\": {\"project\": \"proj-1\","
                       " \"location\": \"us-central1\"}}}") == 0);
    expect_resolved_endpoint("https://us-central1-aiplatform.googleapis.com", "proj-1",
                             "us-central1");

    EXPECT(config_load("{\"providers\": {\"vertex\": {\"project\": \"configured\","
                       " \"location\": \"europe-west1\"}}}") == 0);
    setenv("GOOGLE_CLOUD_PROJECT", "google-primary", 1);
    setenv("ANTHROPIC_VERTEX_PROJECT_ID", "google-alternate", 1);
    setenv("GOOGLE_CLOUD_LOCATION", "asia-east1", 1);
    setenv("CLOUD_ML_REGION", "us-west1", 1);
    expect_resolved_endpoint("https://europe-west1-aiplatform.googleapis.com", "configured",
                             "europe-west1");
    EXPECT_STR_EQ(config_source("providers.vertex.project"), "config");
    EXPECT_STR_EQ(config_source("providers.vertex.location"), "config");

    EXPECT(config_load("{\"providers\": {\"vertex\": {\"project\": \"\","
                       " \"location\": \"\"}}}") == 0);
    expect_resolved_endpoint("https://asia-east1-aiplatform.googleapis.com", "google-primary",
                             "asia-east1");
    EXPECT_STR_EQ(config_source("providers.vertex.project"), "default");
    EXPECT_STR_EQ(config_source("providers.vertex.location"), "default");

    setenv("GOOGLE_CLOUD_PROJECT", "", 1);
    setenv("GOOGLE_CLOUD_LOCATION", "", 1);
    expect_resolved_endpoint("https://us-west1-aiplatform.googleapis.com", "google-alternate",
                             "us-west1");

    setenv("HAX_VERTEX_PROJECT", "hax-project", 1);
    setenv("HAX_VERTEX_LOCATION", "global", 1);
    expect_resolved_endpoint("https://aiplatform.googleapis.com", "hax-project", "global");
    EXPECT_STR_EQ(config_source("providers.vertex.project"), "env");
    EXPECT_STR_EQ(config_source("providers.vertex.location"), "env");

    setenv("HAX_VERTEX_PROJECT", "", 1);
    setenv("HAX_VERTEX_LOCATION", "", 1);
    expect_resolved_endpoint("https://us-west1-aiplatform.googleapis.com", "google-alternate",
                             "us-west1");

    clear_vertex_setting_env();
    setenv("GOOGLE_CLOUD_PROJECT", "default-location", 1);
    EXPECT(config_load("{}") == 0);
    expect_resolved_endpoint("https://us-east5-aiplatform.googleapis.com", "default-location",
                             "us-east5");

    unsetenv("GOOGLE_CLOUD_PROJECT");
    unsigned long diagnostics_before = hax_diag_sequence();
    char *url = vertex_resolve_base_url(NULL);
    EXPECT(url == NULL);
    EXPECT(vertex_resolve_path(NULL) == NULL);
    EXPECT(hax_diag_sequence() == diagnostics_before + 2);
    EXPECT(config_load(NULL) == 0);
}

static void expect_construct_failure(const char *config_json)
{
    EXPECT(config_load(config_json) == 0);
    unsigned long diagnostics_before = hax_diag_sequence();
    struct provider *provider = provider_construct(provider_find("vertex"));
    EXPECT(provider == NULL);
    EXPECT(hax_diag_sequence() == diagnostics_before + 1);
    if (provider)
        provider->destroy(provider);
}

static void test_endpoint_validation(void)
{
    clear_vertex_setting_env();
    expect_construct_failure("{}");
    expect_construct_failure("{\"providers\":{\"vertex\":{"
                             "\"base_url\":\"http://127.0.0.1:1\"}}}");
    expect_construct_failure("{\"providers\":{\"vertex\":{"
                             "\"base_url\":\"http://127.0.0.1:1\","
                             "\"project\":\"bad/project\"}}}");
    expect_construct_failure("{\"providers\":{\"vertex\":{"
                             "\"base_url\":\"http://127.0.0.1:1\","
                             "\"project\":\"valid-project\","
                             "\"location\":\"bad/location\"}}}");

    char long_location[65];
    memset(long_location, 'a', sizeof(long_location) - 1);
    long_location[sizeof(long_location) - 1] = '\0';
    char *config_json = xasprintf("{\"providers\":{\"vertex\":{"
                                  "\"base_url\":\"http://127.0.0.1:1\","
                                  "\"project\":\"valid-project\","
                                  "\"location\":\"%s\"}}}",
                                  long_location);
    expect_construct_failure(config_json);
    free(config_json);

    char max_location[64];
    memset(max_location, 'a', sizeof(max_location) - 1);
    max_location[sizeof(max_location) - 1] = '\0';
    config_json = xasprintf("{\"providers\":{\"vertex\":{"
                            "\"project\":\"valid-project\","
                            "\"location\":\"%s\"}}}",
                            max_location);
    EXPECT(config_load(config_json) == 0);
    free(config_json);
    char *url = vertex_resolve_base_url(NULL);
    char *want = xasprintf("https://%s-aiplatform.googleapis.com", max_location);
    EXPECT_STR_EQ(url, want);
    free(want);
    free(url);
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
    EXPECT_STR_EQ(def->display_name, "google vertex");
    EXPECT_STR_EQ(def->api, "anthropic-messages");
    const struct config_setting *project = config_setting_find("providers.vertex.project");
    const struct config_setting *location = config_setting_find("providers.vertex.location");
    const struct config_setting *token = config_setting_find("providers.vertex.access_token");
    EXPECT(project != NULL && strcmp(project->env_var, "HAX_VERTEX_PROJECT") == 0);
    EXPECT(location != NULL && strcmp(location->env_var, "HAX_VERTEX_LOCATION") == 0);
    EXPECT(token != NULL && strcmp(token->env_var, "HAX_VERTEX_ACCESS_TOKEN") == 0);
    EXPECT(token != NULL && token->secret);
    EXPECT(config_default("providers.vertex.location") == NULL);
    EXPECT_STR_EQ(def->catalog_id, "google-vertex-anthropic");
    EXPECT_STR_EQ(def->version, "vertex-2023-10-16");
    EXPECT(def->body_version == 1);
    EXPECT(def->strict_signatures == 1);
    EXPECT(def->auth_source == vertex_auth_source);
    EXPECT(def->resolve_base_url == vertex_resolve_base_url);
    EXPECT(def->resolve_path == vertex_resolve_path);
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

static void expect_availability(int available, const char *reason)
{
    unsigned long diagnostics_before = hax_diag_sequence();
    struct provider_availability availability = {0};
    provider_prepare_availability(provider_find("vertex"), &availability);
    EXPECT(availability.available == available);
    EXPECT(availability.url == NULL);
    EXPECT(hax_diag_sequence() == diagnostics_before);
    if (reason) {
        EXPECT(availability.reason != NULL);
        if (availability.reason)
            EXPECT_STR_EQ(availability.reason, reason);
    } else {
        EXPECT(availability.reason == NULL);
    }
    provider_availability_clear(&availability);
}

static void test_availability_reasons(void)
{
    struct loopback oauth = {0};
    int port = loopback_listen(&oauth);
    EXPECT(port > 0);
    if (port <= 0)
        return;
    char *url = xasprintf("http://127.0.0.1:%d/token", port);
    setenv("HAX_VERTEX_OAUTH_URL", url, 1);
    free(url);

    char *dir = t_tempdir();
    char *saved_path = t_path_replace(dir);
    char *adc = xasprintf("%s/adc.json", dir);
    setenv("GOOGLE_APPLICATION_CREDENTIALS", adc, 1);
    struct config_snapshot *saved = config_snapshot_take();
    expect_availability(0, "project not set");
    config_set_override("providers.vertex.project", "test-project");
    expect_availability(0, "ADC unavailable");

    const char *malformed = "{not-json}";
    EXPECT(fs_write_atomic(adc, malformed, strlen(malformed), 0) == 0);
    expect_availability(0, "ADC unavailable");
    const char *incomplete = "{\"type\":\"authorized_user\",\"client_id\":\"cid\","
                             "\"refresh_token\":\"refresh\"}";
    EXPECT(fs_write_atomic(adc, incomplete, strlen(incomplete), 0) == 0);
    expect_availability(0, "ADC unavailable");

    const char *user = "{\"type\":\"authorized_user\",\"client_id\":\"cid\","
                       "\"client_secret\":\"secret\",\"refresh_token\":\"refresh\"}";
    EXPECT(fs_write_atomic(adc, user, strlen(user), 0) == 0);
    expect_availability(1, NULL);
    const char *service = "{\"type\":\"service_account\"}";
    EXPECT(fs_write_atomic(adc, service, strlen(service), 0) == 0);
    expect_availability(0, "gcloud not found");

    config_set_override("providers.vertex.access_token", "literal-token");
    expect_availability(1, NULL);
    config_set_override("providers.vertex.access_token", NULL);

    char *gcloud = xasprintf("%s/gcloud", dir);
    char *marker = xasprintf("%s/gcloud-called", dir);
    char *script = xasprintf("#!/bin/sh\n: > '%s'\nexit 1\n", marker);
    EXPECT(fs_write_atomic(gcloud, script, strlen(script), 0) == 0);
    free(script);
    expect_availability(0, "gcloud not found");
    EXPECT(chmod(gcloud, 0700) == 0);
    expect_availability(1, NULL);
    EXPECT(fs_check_regular(marker) != 0);
    struct pollfd listener = {.fd = oauth.listener_fd, .events = POLLIN};
    EXPECT(poll(&listener, 1, 0) == 0);

    free(marker);
    free(gcloud);
    config_snapshot_restore(saved);
    unsetenv("GOOGLE_APPLICATION_CREDENTIALS");
    free(adc);
    t_path_restore(saved_path);
    unsetenv("HAX_VERTEX_OAUTH_URL");
    loopback_stop(&oauth);
}

static void setup_fixtures(void)
{
    char *home = t_tempdir();
    setenv("HOME", home, 1);
    setenv("XDG_CONFIG_HOME", home, 1);
    setenv("XDG_CACHE_HOME", home, 1);
    setenv("CLOUDSDK_CONFIG", home, 1);
    unsetenv("HAX_VERTEX_PROJECT");
    unsetenv("HAX_VERTEX_LOCATION");
    unsetenv("HAX_VERTEX_ACCESS_TOKEN");
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
    test_endpoint_setting_precedence();
    test_endpoint_validation();
    test_def_registered();
    test_messages_body_variant();
    test_stream_raw_predict();
    test_auth_source_user_refresh();
    test_auth_source_literal();
    test_availability_reasons();
    catalog_shutdown();
    config_free();
    curl_global_cleanup();
    T_REPORT();
}
