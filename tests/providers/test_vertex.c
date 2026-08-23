/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "config.h"
#include "harness.h"
#include "provider.h"
#include "util.h"
#include "providers/registry.h"
#include "providers/vertex_auth.h"

struct test_server {
    int listener_fd;
    int request_count; /* how many requests serve_requests accepts before returning */
    const char *response;
    char request_lines[8][512];
    _Atomic int accepted;
};

static void *serve_requests(void *user)
{
    struct test_server *server = user;
    for (int i = 0; i < server->request_count; i++) {
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
                strlen(server->response), server->response);
        close(client_fd);
    }
    return NULL;
}

static int start_server(struct test_server *server, const char *response)
{
    server->response = response;
    server->listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener_fd < 0)
        return -1;

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server->listener_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->listener_fd, 2) != 0) {
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

static char *write_file(const char *name, const char *content)
{
    char *path = xasprintf("%s/%s", t_tempdir(), name);
    FILE *file = fopen(path, "we");
    if (!file) {
        FAIL("fopen(%s): %s", path, strerror(errno));
        free(path);
        return NULL;
    }
    fputs(content, file);
    fclose(file);
    return path;
}

static void test_no_credentials(void)
{
    setenv("GOOGLE_APPLICATION_CREDENTIALS", "/dev/null/nonexistent", 1);
    unsetenv("GOOGLE_OAUTH_ACCESS_TOKEN");
    config_load("{}");

    const char *reason = NULL;
    EXPECT(!vertex_auth_available(&reason));
    EXPECT(reason != NULL);

    char *error = NULL;
    EXPECT(vertex_auth_token(&error) == NULL);
    EXPECT(error != NULL);
    free(error);
    config_free();
}

static void test_literal_token(void)
{
    unsetenv("GOOGLE_APPLICATION_CREDENTIALS");
    unsetenv("GOOGLE_OAUTH_ACCESS_TOKEN");

    config_load("{\"vertex\": {\"access_token\": \"ya29.literal\"}}");
    EXPECT(vertex_auth_available(NULL));

    char *error = NULL;
    const char *token = vertex_auth_token(&error);
    EXPECT(token != NULL);
    if (token)
        EXPECT_STR_EQ(token, "ya29.literal");
    EXPECT(error == NULL);
    free(error);

    /* Invalidate forces re-resolution: the next call must observe the changed literal. */
    config_load("{\"vertex\": {\"access_token\": \"ya29.rotated\"}}");
    vertex_auth_invalidate();
    token = vertex_auth_token(&error);
    EXPECT(token != NULL);
    if (token)
        EXPECT_STR_EQ(token, "ya29.rotated");
    EXPECT(error == NULL);
    free(error);

    vertex_auth_invalidate();
    config_free();
}

static void test_service_account_rejected(void)
{
    char *path =
        write_file("vertex-sa.json", "{\"private_key\": \"-----BEGIN PRIVATE KEY-----\", "
                                     "\"client_email\": \"svc@example.iam.gserviceaccount.com\"}");
    EXPECT(path != NULL);
    unsetenv("GOOGLE_OAUTH_ACCESS_TOKEN");
    setenv("GOOGLE_APPLICATION_CREDENTIALS", path ? path : "", 1);
    config_load("{}");

    const char *reason = NULL;
    EXPECT(!vertex_auth_available(&reason));
    EXPECT(reason != NULL);

    char *error = NULL;
    EXPECT(vertex_auth_token(&error) == NULL);
    EXPECT(error != NULL);
    free(error);

    unsetenv("GOOGLE_APPLICATION_CREDENTIALS");
    config_free();
}

/* User ADC (gcloud) credentials: a refresh token exchange against a loopback token endpoint. */
static void test_user_refresh_flow(void)
{
    struct test_server server = {.request_count = 1};
    int port = start_server(&server, "{\"access_token\":\"ya29.refreshed\",\"expires_in\":3600}");
    if (port < 0) {
        T_SKIP("cannot run a loopback server here");
        return;
    }

    pthread_t thread;
    if (pthread_create(&thread, NULL, serve_requests, &server) != 0) {
        close(server.listener_fd);
        T_SKIP("cannot start a loopback server thread");
        return;
    }

    char *token_url = xasprintf("http://127.0.0.1:%d/token", port);
    setenv("HAX_VERTEX_OAUTH_URL", token_url, 1);
    free(token_url);
    char *adc =
        write_file("vertex-user.json", "{\"client_id\":\"client-1\",\"client_secret\":\"gdf\","
                                       "\"refresh_token\":\"refresh-1\"}");
    EXPECT(adc != NULL);
    setenv("GOOGLE_APPLICATION_CREDENTIALS", adc ? adc : "", 1);
    unsetenv("GOOGLE_OAUTH_ACCESS_TOKEN");
    config_load("{}");

    char *error = NULL;
    const char *token = vertex_auth_token(&error);
    EXPECT(token != NULL);
    EXPECT(error == NULL);
    if (token)
        EXPECT_STR_EQ(token, "ya29.refreshed");
    free(error);

    pthread_join(thread, NULL);
    close(server.listener_fd);
    unsetenv("GOOGLE_APPLICATION_CREDENTIALS");
    unsetenv("HAX_VERTEX_OAUTH_URL");
    vertex_auth_invalidate();
    config_free();
}

static void test_registry(void)
{
    EXPECT(provider_find("vertex") != NULL);
    unsetenv("GOOGLE_APPLICATION_CREDENTIALS");
    unsetenv("GOOGLE_OAUTH_ACCESS_TOKEN");
    const struct provider_factory *factory = provider_find("vertex");

    setenv("HAX_VERTEX_BASE_URL", "http://127.0.0.1:1/v1/...:streamRawPredict", 1);
    config_load("{}");
    struct provider *provider = factory ? factory->new(factory->id) : NULL;
    EXPECT(provider != NULL);
    if (provider)
        provider->destroy(provider);
    unsetenv("HAX_VERTEX_BASE_URL");
    config_free();
}

int main(void)
{
    unsetenv("HAX_VERTEX_OAUTH_URL");
    test_no_credentials();
    test_literal_token();
    test_service_account_rejected();
    test_user_refresh_flow();
    test_registry();
    T_REPORT();
}
