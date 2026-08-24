/* SPDX-License-Identifier: MIT */
#include <jansson.h>
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
#include "provider.h"
#include "providers/http_provider.h"

struct test_server {
    int listener_fd;
    const char *response;
    char request[2048];
    _Atomic int responses_sent;
};

static void *serve_response(void *user)
{
    struct test_server *server = user;
    struct pollfd poll_fd = {.fd = server->listener_fd, .events = POLLIN};
    if (poll(&poll_fd, 1, 10000) <= 0)
        return NULL;

    int client_fd = accept(server->listener_fd, NULL, NULL);
    if (client_fd < 0)
        return NULL;

    ssize_t bytes_read = read(client_fd, server->request, sizeof(server->request) - 1);
    if (bytes_read > 0)
        server->request[bytes_read] = '\0';

    dprintf(client_fd, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
            strlen(server->response), server->response);
    close(client_fd);
    atomic_fetch_add(&server->responses_sent, 1);
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
        listen(server->listener_fd, 1) != 0) {
        close(server->listener_fd);
        server->listener_fd = -1;
        return -1;
    }

    socklen_t length = sizeof(address);
    if (getsockname(server->listener_fd, (struct sockaddr *)&address, &length) != 0) {
        close(server->listener_fd);
        server->listener_fd = -1;
        return -1;
    }
    return ntohs(address.sin_port);
}

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
    struct test_server server = {.response = response};
    int port = start_server(&server);
    if (port < 0)
        return -2;

    pthread_t thread;
    if (pthread_create(&thread, NULL, serve_response, &server) != 0) {
        close(server.listener_fd);
        return -2;
    }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);
    struct http_provider_preset preset = {
        .display_name = "flat",
        .default_base_url = base_url,
        .config_prefix = "providers.flat",
        .parse_model = parse_context_length,
    };
    config_set_override("providers.flat.api_key", "sk-flat");
    struct provider *provider = http_provider_new_preset(&preset);
    config_set_override("providers.flat.api_key", NULL);
    EXPECT(provider != NULL);

    int result = -2;
    if (provider) {
        result = provider->list_models(provider, models, n_models, error, NULL, NULL);
        provider->destroy(provider);
    }
    pthread_join(thread, NULL);
    close(server.listener_fd);

    /* The listing authenticates with the OpenAI-side Bearer scheme. */
    if (atomic_load(&server.responses_sent) == 1)
        EXPECT(strstr(server.request, "Authorization: Bearer sk-flat\r\n") != NULL);
    return result;
}

/* Usable ids are listed in server order, refined by the preset's parse hook; an id-less entry
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

int main(void)
{
    test_lists_flat_models();
    test_null_data_is_empty_success();
    test_unrecognized_shape_reports_error();
    T_REPORT();
}
