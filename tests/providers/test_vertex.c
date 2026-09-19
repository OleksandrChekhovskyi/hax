/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <jansson.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "config.h"
#include "diag.h"
#include "harness.h"
#include "provider.h"
#include "xalloc.h"
#include "providers/anthropic_body.h"
#include "providers/http_provider.h"
#include "providers/registry.h"
#include "providers/vertex.h"
#include "providers/wire.h"

/* Endpoint host follows the resolved location: `global`, a `us`/`eu` multi-region, or the
 * regional host. An explicit base_url would win verbatim instead. */
static void test_resolve_base_url_host_rules(void)
{
    /* Host env leaking in would override the registry defaults the test leans on. */
    unsetenv("GOOGLE_CLOUD_PROJECT");
    unsetenv("ANTHROPIC_VERTEX_PROJECT_ID");
    unsetenv("GOOGLE_CLOUD_LOCATION");
    unsetenv("CLOUD_ML_REGION");
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

/* One sequential-connection server echoing a canned reply; captures each request. */
struct wire_server {
    int listener_fd;
    const char *response;
    int n_requests;
    char requests[8][8192];
    _Atomic int served;
};

static void *serve_requests(void *user)
{
    struct wire_server *server = user;
    for (int i = 0; i < server->n_requests; i++) {
        struct pollfd poll_fd = {.fd = server->listener_fd, .events = POLLIN};
        if (poll(&poll_fd, 1, 10000) <= 0)
            return NULL;
        int client_fd = accept(server->listener_fd, NULL, NULL);
        if (client_fd < 0)
            return NULL;
        char *request = server->requests[i];
        size_t request_len = 0;
        size_t expected_len = 0;
        while (request_len < sizeof(server->requests[i]) - 1) {
            ssize_t bytes_read = read(client_fd, request + request_len,
                                      sizeof(server->requests[i]) - request_len - 1);
            if (bytes_read <= 0)
                break;
            request_len += (size_t)bytes_read;
            request[request_len] = '\0';
            char *header_end = strstr(request, "\r\n\r\n");
            if (header_end && expected_len == 0) {
                const char *length = strstr(request, "Content-Length: ");
                expected_len = (size_t)(header_end + 4 - request) +
                               (length ? strtoul(length + 16, NULL, 10) : 0);
            }
            if (expected_len > 0 && request_len >= expected_len)
                break;
        }
        const char *response = server->response;
        size_t response_len = strlen(response);
        size_t written = 0;
        while (written < response_len) {
            ssize_t result = write(client_fd, response + written, response_len - written);
            if (result <= 0)
                break;
            written += (size_t)result;
        }
        close(client_fd);
        atomic_fetch_add(&server->served, 1);
    }
    return NULL;
}

static int start_server(struct wire_server *server, pthread_t *thread)
{
    server->listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener_fd < 0)
        return -1;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server->listener_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->listener_fd, 8) != 0)
        goto error;
    socklen_t address_len = sizeof(address);
    if (getsockname(server->listener_fd, (struct sockaddr *)&address, &address_len) != 0)
        goto error;
    if (pthread_create(thread, NULL, serve_requests, server) != 0)
        goto error;
    return ntohs(address.sin_port);
error:
    close(server->listener_fd);
    server->listener_fd = -1;
    return -1;
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
    struct wire_server server = {
        .response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 2\r\nConnection: close\r\n\r\nno",
        .n_requests = 1,
    };
    pthread_t thread;
    int port = start_server(&server, &thread);
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
        return;

    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = "hello"}};
    struct context context = {.items = items, .n_items = 1, .image_input = 1};
    struct error_log log = {0};
    const char *model = "claude-sonnet@20250929";
    provider->stream(provider, &context, model, log_error, &log, NULL, NULL);
    pthread_join(thread, NULL);
    close(server.listener_fd);
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

    provider->destroy(provider);
    EXPECT(config_load(NULL) == 0);
}

/* A minimal HTTP body that reads the request and returns a canned response; used to stand in for
 * the OAuth token endpoint. */
struct oauth_server {
    int listener_fd;
    const char *response;
    int n_requests;
    char request[8192];
    _Atomic int served;
};

static void *serve_oauth(void *user)
{
    struct oauth_server *server = user;
    for (int i = 0; i < server->n_requests; i++) {
        struct pollfd poll_fd = {.fd = server->listener_fd, .events = POLLIN};
        if (poll(&poll_fd, 1, 10000) <= 0)
            return NULL;
        int client_fd = accept(server->listener_fd, NULL, NULL);
        if (client_fd < 0)
            return NULL;
        ssize_t bytes = read(client_fd, server->request, sizeof(server->request) - 1);
        if (bytes > 0)
            server->request[bytes] = '\0';
        const char *response = server->response;
        dprintf(client_fd, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                strlen(response), response);
        close(client_fd);
        atomic_fetch_add(&server->served, 1);
    }
    return NULL;
}

static int start_oauth_server(struct oauth_server *server, pthread_t *thread, const char *response)
{
    server->response = response;
    server->listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener_fd < 0)
        return -1;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server->listener_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->listener_fd, 2) != 0)
        goto error;
    socklen_t address_len = sizeof(address);
    if (getsockname(server->listener_fd, (struct sockaddr *)&address, &address_len) != 0)
        goto error;
    if (pthread_create(thread, NULL, serve_oauth, server) != 0)
        goto error;
    return ntohs(address.sin_port);
error:
    close(server->listener_fd);
    server->listener_fd = -1;
    return -1;
}

/* Write a fresh authorized_user ADC file into a temp dir and hand the path out via
 * GOOGLE_APPLICATION_CREDENTIALS. */
static void write_adc_user_file(const char *refresh_token)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/adc.json", dir);
    FILE *f = fopen(path, "w");
    if (!f)
        FAIL("fopen %s: %s", path, strerror(errno));
    fputs("{\"type\": \"authorized_user\","
          "\"client_id\": \"cid\","
          "\"client_secret\": \"csecret\","
          "\"refresh_token\": \"",
          f);
    fputs(refresh_token, f);
    fputs("\"}", f);
    fclose(f);
    setenv("GOOGLE_APPLICATION_CREDENTIALS", path, 1);
    free(path);
}

/* Prompt-less auth through an authorized_user ADC file: a 401 to one crafted to expiring
 * credentials recovers by refreshing against the (overridable) token endpoint, and the rebuilt
 * request carries the new token. */
static void test_auth_source_user_refresh(void)
{
    struct oauth_server oauth = {.n_requests = 2};
    char oauth_url[64];
    pthread_t oauth_thread;
    int oauth_port = start_oauth_server(&oauth, &oauth_thread,
                                        "{\"access_token\": \"fresh-2\", \"expires_in\": 3600}");
    EXPECT(oauth_port > 0);
    if (oauth_port <= 0)
        return;
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

    pthread_join(oauth_thread, NULL);
    close(oauth.listener_fd);
    EXPECT(atomic_load(&oauth.served) == 2);
    /* The refresh is a form POST carrying the ADC's own refresh token. */
    EXPECT(strstr(oauth.request, "grant_type=refresh_token") != NULL);
    EXPECT(strstr(oauth.request, "refresh_token=old-refresh") != NULL);

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

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_resolve_base_url_host_rules();
    test_def_registered();
    test_messages_body_variant();
    test_stream_raw_predict();
    test_auth_source_user_refresh();
    test_auth_source_literal();
    T_REPORT();
}
