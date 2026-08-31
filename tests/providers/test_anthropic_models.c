/* SPDX-License-Identifier: MIT */
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "config.h"
#include "harness.h"
#include "model_meta.h"
#include "provider.h"
#include "xalloc.h"
#include "providers/http_provider.h"
#include "providers/registry.h"

#define MAX_PAGES 4

struct test_server {
    int listener_fd;
    const char *responses[MAX_PAGES];
    int n_responses;
    char request_lines[MAX_PAGES][512];
    _Atomic int responses_sent;
};

static void *serve_responses(void *user)
{
    struct test_server *server = user;
    for (int i = 0; i < server->n_responses; i++) {
        struct pollfd poll_fd = {.fd = server->listener_fd, .events = POLLIN};
        if (poll(&poll_fd, 1, 10000) <= 0)
            return NULL;

        int client_fd = accept(server->listener_fd, NULL, NULL);
        if (client_fd < 0)
            return NULL;

        char request[2048] = {0};
        ssize_t bytes_read = read(client_fd, request, sizeof(request) - 1);
        if (bytes_read > 0) {
            char *line_end = strstr(request, "\r\n");
            size_t line_length = line_end ? (size_t)(line_end - request) : strlen(request);
            if (line_length >= sizeof(server->request_lines[i]))
                line_length = sizeof(server->request_lines[i]) - 1;
            memcpy(server->request_lines[i], request, line_length);
            server->request_lines[i][line_length] = '\0';
        }

        dprintf(client_fd, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                strlen(server->responses[i]), server->responses[i]);
        close(client_fd);
        atomic_fetch_add(&server->responses_sent, 1);
    }
    return NULL;
}

static int start_server(struct test_server *server)
{
    server->listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener_fd < 0)
        return -1;

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server->listener_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->listener_fd, MAX_PAGES) != 0) {
        goto fail;
    }

    socklen_t length = sizeof(address);
    if (getsockname(server->listener_fd, (struct sockaddr *)&address, &length) != 0)
        goto fail;
    return ntohs(address.sin_port);

fail:
    close(server->listener_fd);
    server->listener_fd = -1;
    return -1;
}

static int list_models_from_server(struct test_server *server, int n_responses, char **model_ids,
                                   size_t max_model_ids, size_t *n_model_ids)
{
    *n_model_ids = 0;
    server->n_responses = n_responses;
    int port = start_server(server);
    if (port < 0)
        return -1;

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);
    setenv("HAX_ANTHROPIC_BASE_URL", base_url, 1);

    pthread_t thread;
    if (pthread_create(&thread, NULL, serve_responses, server) != 0) {
        close(server->listener_fd);
        return -1;
    }

    const struct provider_def *factory = provider_find("anthropic-compatible");
    EXPECT(factory != NULL);
    struct provider *provider = factory ? provider_construct(factory) : NULL;
    EXPECT(provider != NULL);

    size_t n_models = 0;
    if (provider && provider->list_models) {
        struct model_info *models = NULL;
        char *error = NULL;
        int result = provider->list_models(provider, &models, &n_models, &error, NULL, NULL);
        EXPECT(result == 0);
        for (size_t i = 0; i < n_models && i < max_model_ids; i++)
            model_ids[i] = xstrdup(models[i].id);
        model_info_free(models, n_models);
        free(error);
    }
    if (provider)
        provider->destroy(provider);
    pthread_join(thread, NULL);
    close(server->listener_fd);
    *n_model_ids = n_models;
    return 0;
}

static void test_follows_cursor(void)
{
    struct test_server server = {0};
    server.responses[0] = "{\"data\":[{\"id\":\"m1\"},{\"id\":\"m2\"}],"
                          "\"has_more\":true,\"last_id\":\"m2\"}";
    server.responses[1] = "{\"data\":[{\"id\":\"m3\"}],\"has_more\":false,\"last_id\":\"m3\"}";

    char *model_ids[8] = {0};
    size_t n_models;
    if (list_models_from_server(&server, 2, model_ids, 8, &n_models) != 0)
        T_SKIP("cannot run a loopback server here");
    EXPECT(n_models == 3);
    if (n_models == 3) {
        EXPECT_STR_EQ(model_ids[0], "m1");
        EXPECT_STR_EQ(model_ids[1], "m2");
        EXPECT_STR_EQ(model_ids[2], "m3");
    }

    EXPECT(strstr(server.request_lines[0], "limit=") != NULL);
    EXPECT(strstr(server.request_lines[0], "after_id=") == NULL);
    EXPECT(strstr(server.request_lines[1], "after_id=m2") != NULL);
    for (size_t i = 0; i < n_models; i++)
        free(model_ids[i]);
}

static void test_repeated_cursor_page_is_discarded(void)
{
    struct test_server server = {0};
    server.responses[0] = "{\"data\":[{\"id\":\"m1\"},{\"id\":\"m2\"}],"
                          "\"has_more\":true,\"last_id\":\"m2\"}";
    server.responses[1] = "{\"data\":[{\"id\":\"m1\"},{\"id\":\"m2\"}],"
                          "\"has_more\":true,\"last_id\":\"m2\"}";

    char *model_ids[8] = {0};
    size_t n_models;
    if (list_models_from_server(&server, 2, model_ids, 8, &n_models) != 0)
        T_SKIP("cannot run a loopback server here");
    EXPECT(n_models == 2);
    if (n_models == 2) {
        EXPECT_STR_EQ(model_ids[0], "m1");
        EXPECT_STR_EQ(model_ids[1], "m2");
    }
    for (size_t i = 0; i < n_models; i++)
        free(model_ids[i]);
}

static void test_missing_cursor_stops(void)
{
    struct test_server server = {0};
    server.responses[0] = "{\"data\":[{\"id\":\"m1\"}],\"has_more\":true}";

    char *model_ids[8] = {0};
    size_t n_models;
    if (list_models_from_server(&server, 1, model_ids, 8, &n_models) != 0)
        T_SKIP("cannot run a loopback server here");
    EXPECT(n_models == 1);
    if (n_models == 1)
        EXPECT_STR_EQ(model_ids[0], "m1");
    for (size_t i = 0; i < n_models; i++)
        free(model_ids[i]);
}

static void store_output_cap(struct provider *provider, const char *model, long max_output)
{
    struct model_info metadata;
    model_info_init(&metadata);
    metadata.id = xstrdup(model);
    metadata.max_output = max_output;
    model_meta_store(provider, &metadata);
    model_info_clear(&metadata);
}

static void test_background_probe_publishes_metadata(void)
{
    struct test_server server = {.n_responses = 1};
    server.responses[0] = "{\"data\":[{\"id\":\"probe-model\",\"max_input_tokens\":12345,"
                          "\"max_tokens\":6789}]}";
    int port = start_server(&server);
    if (port < 0)
        T_SKIP("cannot run a loopback server here");

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", port);
    setenv("HAX_ANTHROPIC_BASE_URL", url, 1);
    setenv("HAX_MODEL", "probe-model", 1);

    pthread_t thread;
    if (pthread_create(&thread, NULL, serve_responses, &server) != 0) {
        close(server.listener_fd);
        unsetenv("HAX_MODEL");
        T_SKIP("cannot start a loopback server thread");
    }

    const struct provider_def *factory = provider_find("anthropic-compatible");
    EXPECT(factory != NULL);
    struct provider *provider = factory ? provider_construct(factory) : NULL;
    EXPECT(provider != NULL);
    if (provider) {
        model_meta_wait(provider);
        EXPECT(model_meta_context(provider, "probe-model") == 12345);
        EXPECT(model_meta_max_output(provider, "probe-model") == 6789);

        model_meta_refresh(provider, "probe-model");
        struct model_info snapshot;
        EXPECT(model_meta_snapshot(provider, &snapshot) == 1);
        EXPECT_STR_EQ(snapshot.id, "probe-model");
        model_info_clear(&snapshot);
        provider->destroy(provider);
    }

    pthread_join(thread, NULL);
    close(server.listener_fd);
    EXPECT(atomic_load(&server.responses_sent) == 1);
    unsetenv("HAX_MODEL");
}

static void test_max_tokens_uses_model_limit(void)
{
    unsetenv("HAX_ANTHROPIC_MAX_TOKENS");

    EXPECT(config_str("providers.anthropic-compatible.max_tokens") == NULL);
    EXPECT(config_int("providers.anthropic-compatible.max_tokens") == 0);

    setenv("HAX_ANTHROPIC_BASE_URL", "http://127.0.0.1:1", 1);
    const struct provider_def *factory = provider_find("anthropic-compatible");
    EXPECT(factory != NULL);
    struct provider *provider = factory ? provider_construct(factory) : NULL;
    EXPECT(provider != NULL);
    if (!provider)
        return;

    EXPECT(http_provider_max_tokens(provider, "unknown-model") == 32000);

    store_output_cap(provider, "claude-opus-5", 128000);
    EXPECT(http_provider_max_tokens(provider, "claude-opus-5") == 128000);

    setenv("HAX_ANTHROPIC_MAX_TOKENS", "8000", 1);
    EXPECT(http_provider_max_tokens(provider, "claude-opus-5") == 8000);

    setenv("HAX_ANTHROPIC_MAX_TOKENS", "200000", 1);
    EXPECT(http_provider_max_tokens(provider, "claude-opus-5") == 128000);

    EXPECT(http_provider_max_tokens(provider, "unknown-model") == 200000);
    unsetenv("HAX_ANTHROPIC_MAX_TOKENS");
    provider->destroy(provider);
}

/* First-party identity is pinned: providers.anthropic.base_url is ignored, while tweak fields
 * such as max_tokens resolve from that same block. */
static void test_first_party_pins_endpoint(void)
{
    config_set_override("providers.anthropic.base_url", "http://127.0.0.1:1");
    config_set_override("providers.anthropic.max_tokens", "1234");

    struct provider *provider = provider_construct(provider_find("anthropic"));
    EXPECT(provider != NULL);
    if (provider) {
        struct model_probe probe = {0};
        EXPECT(provider->probe_model(provider, "claude-x", &probe) == 0);
        EXPECT(probe.url != NULL &&
               strncmp(probe.url, "https://api.anthropic.com/v1/models", 35) == 0);
        model_probe_clear(&probe);
        EXPECT(http_provider_max_tokens(provider, "claude-x") == 1234);
        provider->destroy(provider);
    }

    config_set_override("providers.anthropic.max_tokens", NULL);
    config_set_override("providers.anthropic.base_url", NULL);
}

int main(void)
{
    setenv("HAX_ANTHROPIC_API_KEY", "test-key", 1);

    /* Keep constructor probes from racing the model-list fixture for its canned response. */
    unsetenv("HAX_MODEL");
    test_first_party_pins_endpoint();
    test_max_tokens_uses_model_limit();
    test_background_probe_publishes_metadata();
    test_follows_cursor();
    test_repeated_cursor_page_is_discarded();
    test_missing_cursor_stops();
    T_REPORT();
}
