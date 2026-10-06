/* SPDX-License-Identifier: MIT */
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "harness.h"
#include "loopback.h"
#include "transport/http.h"

static void make_url(char *url, size_t size, int port)
{
    snprintf(url, size, "http://127.0.0.1:%d/test", port);
}

static void test_get_response(void)
{
    struct loopback server = {
        .response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello",
    };
    int port = loopback_start(&server);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    const char *headers[] = {"X-Test: transport", NULL};
    char *body = NULL;
    long status = 0;
    int result = http_get(url, headers, 2, 0, NULL, NULL, &body, &status);
    loopback_stop(&server);

    EXPECT(result == 0);
    EXPECT(status == 200);
    EXPECT_STR_EQ(body, "hello");
    EXPECT(strstr(server.requests[0], "GET /test HTTP/") != NULL);
    EXPECT(strstr(server.requests[0], "X-Test: transport\r\n") != NULL);
    free(body);
}

static void test_get_offers_compression(void)
{
    if (!(curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_LIBZ))
        T_SKIP("libcurl lacks zlib");
    struct loopback server = {
        .response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}",
    };
    int port = loopback_start(&server);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    char *body = NULL;
    int result = http_get(url, NULL, 2, 0, NULL, NULL, &body, NULL);
    loopback_stop(&server);

    EXPECT(result == 0);
    char accepted[128] = "";
    const char *header = strstr(server.requests[0], "Accept-Encoding: ");
    if (header)
        sscanf(header, "Accept-Encoding: %127[^\r]", accepted);
    EXPECT(strstr(accepted, "gzip") != NULL);
    free(body);
}

static void test_json_post(void)
{
    static const char request_body[] = "{\"model\":\"x\"}";
    struct loopback server = {
        .response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}",
    };
    int port = loopback_start(&server);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    char *body = NULL;
    int result =
        http_post_json(url, NULL, request_body, sizeof(request_body) - 1, 2, 0, NULL, NULL, &body);
    loopback_stop(&server);

    EXPECT(result == 0);
    EXPECT_STR_EQ(body, "{}");
    EXPECT(strstr(server.requests[0], "POST /test HTTP/") != NULL);
    EXPECT(strstr(server.requests[0], "Content-Type: application/json\r\n") != NULL);
    EXPECT(strstr(server.requests[0], "\r\n\r\n{\"model\":\"x\"}") != NULL);
    free(body);
}

static void test_response_size_limit(void)
{
    struct loopback server = {
        .response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello",
    };
    int port = loopback_start(&server);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    char *body = NULL;
    long status = 0;
    int result = http_get(url, NULL, 2, 4, NULL, NULL, &body, &status);
    loopback_stop(&server);

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
    struct loopback server = {
        .response = "HTTP/1.1 200 OK\r\nContent-Length: 28\r\nConnection: close\r\n\r\n"
                    "event: message\ndata: hello\n\n",
    };
    int port = loopback_start(&server);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    struct event_capture capture = {0};
    struct http_response response;
    int result =
        http_sse_post(url, NULL, "{}", 2, 2, capture_event, &capture, NULL, NULL, &response);
    loopback_stop(&server);

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
    struct loopback server = {
        .response = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 2\r\nContent-Length: 24\r\n"
                    "Connection: close\r\n\r\ndata: {\"error\":\"busy\"}\n\n",
    };
    int port = loopback_start(&server);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    struct event_capture capture = {0};
    struct http_response response;
    int result =
        http_sse_post(url, NULL, "{}", 2, 2, capture_event, &capture, NULL, NULL, &response);
    loopback_stop(&server);

    EXPECT(result == 0);
    EXPECT(response.status == 503);
    EXPECT(response.retry_after_ms == 2000);
    EXPECT_STR_EQ(response.error_body, error_body);
    EXPECT(capture.count == 0);
    free(response.error_body);
}

struct cancel_state {
    struct loopback *server;
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
    struct loopback server = {
        .response = "HTTP/1.1 200 OK\r\nContent-Length: 28\r\nConnection: close\r\n\r\n"
                    "event: message\ndata: hello\n\n",
    };
    int port = loopback_start(&server);
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
    loopback_stop(&server);

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
    struct loopback server = {
        .response = "HTTP/1.1 403 Forbidden\r\nContent-Length: 18\r\nConnection: close\r\n\r\n"
                    "{\"error\":\"denied\"}",
    };
    int port = loopback_start(&server);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    char *body = NULL;
    long status = 0;
    int result = http_post(url, NULL, "application/x-www-form-urlencoded", request_body,
                           sizeof(request_body) - 1, 2, 0, NULL, NULL, &body, &status);
    loopback_stop(&server);

    EXPECT(result == 0);
    EXPECT(status == 403);
    EXPECT(body != NULL);
    if (body)
        EXPECT(strstr(body, "denied") != NULL);
    EXPECT(strstr(server.requests[0], "Content-Type: application/x-www-form-urlencoded\r\n") !=
           NULL);
    EXPECT(strstr(server.requests[0], "\r\n\r\ngrant_type=authorization_code&code=abc") != NULL);
    free(body);
}

static void test_post_empty_body_is_null(void)
{
    struct loopback server = {
        .response = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
    };
    int port = loopback_start(&server);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char url[64];
    make_url(url, sizeof(url), port);
    char *body = NULL;
    long status = 0;
    int result = http_post(url, NULL, NULL, NULL, 0, 2, 0, NULL, NULL, &body, &status);
    loopback_stop(&server);

    EXPECT(result == 0);
    EXPECT(status == 204);
    EXPECT(body == NULL);
    free(body);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_get_response();
    test_get_offers_compression();
    test_json_post();
    test_post_exposes_error_status();
    test_post_empty_body_is_null();
    test_response_size_limit();
    test_sse_success();
    test_sse_error_response();
    test_sse_cancellation();
    T_REPORT();
}
