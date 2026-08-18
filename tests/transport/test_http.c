/* SPDX-License-Identifier: MIT */
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

#include "harness.h"
#include "transport/http.h"

struct test_server {
    int listener_fd;
    const char *response;
    size_t request_body_len;
    char request[4096];
    _Atomic int accepted;
};

static void *serve_once(void *user)
{
    struct test_server *server = user;
    struct pollfd poll_fd = {.fd = server->listener_fd, .events = POLLIN};
    if (poll(&poll_fd, 1, 10000) <= 0)
        return NULL;

    int client_fd = accept(server->listener_fd, NULL, NULL);
    if (client_fd < 0)
        return NULL;
    atomic_store(&server->accepted, 1);

    size_t request_len = 0;
    size_t expected_len = 0;
    while (request_len < sizeof(server->request) - 1) {
        ssize_t bytes_read = read(client_fd, server->request + request_len,
                                  sizeof(server->request) - request_len - 1);
        if (bytes_read <= 0)
            break;
        request_len += (size_t)bytes_read;
        server->request[request_len] = '\0';

        char *header_end = strstr(server->request, "\r\n\r\n");
        if (header_end && expected_len == 0)
            expected_len = (size_t)(header_end + 4 - server->request) + server->request_body_len;
        if (expected_len > 0 && request_len >= expected_len)
            break;
    }

    size_t response_len = strlen(server->response);
    size_t written = 0;
    while (written < response_len) {
        ssize_t result = write(client_fd, server->response + written, response_len - written);
        if (result <= 0)
            break;
        written += (size_t)result;
    }
    close(client_fd);
    return NULL;
}

static int start_server(struct test_server *server, pthread_t *thread)
{
    server->listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener_fd < 0)
        return -1;

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server->listener_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->listener_fd, 1) != 0)
        goto error;

    socklen_t address_len = sizeof(address);
    if (getsockname(server->listener_fd, (struct sockaddr *)&address, &address_len) != 0)
        goto error;
    if (pthread_create(thread, NULL, serve_once, server) != 0)
        goto error;
    return ntohs(address.sin_port);

error:
    close(server->listener_fd);
    server->listener_fd = -1;
    return -1;
}

static void stop_server(struct test_server *server, pthread_t thread)
{
    pthread_join(thread, NULL);
    close(server->listener_fd);
}

static void make_url(char *url, size_t size, int port)
{
    snprintf(url, size, "http://127.0.0.1:%d/test", port);
}

static void test_get_response(void)
{
    struct test_server server = {
        .response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello",
    };
    pthread_t thread;
    int port = start_server(&server, &thread);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    const char *headers[] = {"X-Test: transport", NULL};
    char *body = NULL;
    long status = 0;
    int result = http_get(url, headers, 2, 0, NULL, NULL, &body, &status);
    stop_server(&server, thread);

    EXPECT(result == 0);
    EXPECT(status == 200);
    EXPECT_STR_EQ(body, "hello");
    EXPECT(strstr(server.request, "GET /test HTTP/") != NULL);
    EXPECT(strstr(server.request, "X-Test: transport\r\n") != NULL);
    free(body);
}

static void test_json_post(void)
{
    static const char request_body[] = "{\"model\":\"x\"}";
    struct test_server server = {
        .response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}",
        .request_body_len = sizeof(request_body) - 1,
    };
    pthread_t thread;
    int port = start_server(&server, &thread);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    char *body = NULL;
    int result =
        http_post_json(url, NULL, request_body, sizeof(request_body) - 1, 2, 0, NULL, NULL, &body);
    stop_server(&server, thread);

    EXPECT(result == 0);
    EXPECT_STR_EQ(body, "{}");
    EXPECT(strstr(server.request, "POST /test HTTP/") != NULL);
    EXPECT(strstr(server.request, "Content-Type: application/json\r\n") != NULL);
    EXPECT(strstr(server.request, "\r\n\r\n{\"model\":\"x\"}") != NULL);
    free(body);
}

static void test_response_size_limit(void)
{
    struct test_server server = {
        .response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello",
    };
    pthread_t thread;
    int port = start_server(&server, &thread);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    char *body = NULL;
    long status = 0;
    int result = http_get(url, NULL, 2, 4, NULL, NULL, &body, &status);
    stop_server(&server, thread);

    EXPECT(result == -1);
    EXPECT(status == 200);
    EXPECT(body == NULL);
}

struct event_capture {
    int count;
    char event_name[32];
    char data[128];
};

static int capture_event(const char *event_name, const char *data, void *user)
{
    struct event_capture *capture = user;
    capture->count++;
    snprintf(capture->event_name, sizeof(capture->event_name), "%s", event_name);
    snprintf(capture->data, sizeof(capture->data), "%s", data);
    return 0;
}

static void test_sse_success(void)
{
    struct test_server server = {
        .response = "HTTP/1.1 200 OK\r\nContent-Length: 28\r\nConnection: close\r\n\r\n"
                    "event: message\ndata: hello\n\n",
    };
    pthread_t thread;
    int port = start_server(&server, &thread);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    struct event_capture capture = {0};
    struct http_response response;
    int result =
        http_sse_post(url, NULL, "{}", 2, 2, capture_event, &capture, NULL, NULL, &response);
    stop_server(&server, thread);

    EXPECT(result == 0);
    EXPECT(response.status == 200);
    EXPECT(response.error_body == NULL);
    EXPECT(capture.count == 1);
    EXPECT_STR_EQ(capture.event_name, "message");
    EXPECT_STR_EQ(capture.data, "hello");
    free(response.error_body);
}

static void test_sse_error_response(void)
{
    static const char error_body[] = "data: {\"error\":\"busy\"}\n\n";
    struct test_server server = {
        .response = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 2\r\nContent-Length: 24\r\n"
                    "Connection: close\r\n\r\ndata: {\"error\":\"busy\"}\n\n",
    };
    pthread_t thread;
    int port = start_server(&server, &thread);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    struct event_capture capture = {0};
    struct http_response response;
    int result =
        http_sse_post(url, NULL, "{}", 2, 2, capture_event, &capture, NULL, NULL, &response);
    stop_server(&server, thread);

    EXPECT(result == 0);
    EXPECT(response.status == 503);
    EXPECT(response.retry_after_ms == 2000);
    EXPECT_STR_EQ(response.error_body, error_body);
    EXPECT(capture.count == 0);
    free(response.error_body);
}

struct cancel_state {
    struct test_server *server;
    int calls;
};

static int cancel_after_connect(void *user)
{
    struct cancel_state *cancel = user;
    cancel->calls++;
    return atomic_load(&cancel->server->accepted);
}

static void test_sse_cancellation(void)
{
    struct test_server server = {
        .response = "HTTP/1.1 200 OK\r\nContent-Length: 28\r\nConnection: close\r\n\r\n"
                    "event: message\ndata: hello\n\n",
    };
    pthread_t thread;
    int port = start_server(&server, &thread);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    struct event_capture capture = {0};
    struct cancel_state cancel = {.server = &server};
    struct http_response response;
    int result = http_sse_post(url, NULL, "{}", 2, 2, capture_event, &capture, cancel_after_connect,
                               &cancel, &response);
    stop_server(&server, thread);

    EXPECT(result == -1);
    EXPECT(response.cancelled == 1);
    EXPECT(response.error_body == NULL);
    EXPECT(cancel.calls > 0);
    EXPECT(capture.count == 0);
    free(response.error_body);
}

/* OAuth endpoints report state through non-2xx JSON, so http_post must surface the status and
 * body instead of collapsing them into -1 like the other buffered helpers. */
static void test_post_exposes_error_status(void)
{
    static const char request_body[] = "grant_type=authorization_code&code=abc";
    struct test_server server = {
        .response = "HTTP/1.1 403 Forbidden\r\nContent-Length: 18\r\nConnection: close\r\n\r\n"
                    "{\"error\":\"denied\"}",
        .request_body_len = sizeof(request_body) - 1,
    };
    pthread_t thread;
    int port = start_server(&server, &thread);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    char *body = NULL;
    long status = 0;
    int result = http_post(url, NULL, "application/x-www-form-urlencoded", request_body,
                           sizeof(request_body) - 1, 2, 0, NULL, NULL, &body, &status);
    stop_server(&server, thread);

    EXPECT(result == 0);
    EXPECT(status == 403);
    EXPECT(body != NULL);
    if (body)
        EXPECT(strstr(body, "denied") != NULL);
    EXPECT(strstr(server.request, "Content-Type: application/x-www-form-urlencoded\r\n") != NULL);
    EXPECT(strstr(server.request, "\r\n\r\ngrant_type=authorization_code&code=abc") != NULL);
    free(body);
}

static void test_post_empty_body_is_null(void)
{
    struct test_server server = {
        .response = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
    };
    pthread_t thread;
    int port = start_server(&server, &thread);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    char *body = NULL;
    long status = 0;
    int result = http_post(url, NULL, NULL, NULL, 0, 2, 0, NULL, NULL, &body, &status);
    stop_server(&server, thread);

    EXPECT(result == 0);
    EXPECT(status == 204);
    EXPECT(body == NULL);
    free(body);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_get_response();
    test_json_post();
    test_post_exposes_error_status();
    test_post_empty_body_is_null();
    test_response_size_limit();
    test_sse_success();
    test_sse_error_response();
    test_sse_cancellation();
    T_REPORT();
}
