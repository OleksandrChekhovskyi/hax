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
#include "provider.h"
#include "util.h"
#include "providers/stream_retry.h"
#include "transport/http.h"

#define REQUEST_BODY "{\"model\":\"m\"}"

#define SSE_OK                                                                                     \
    "HTTP/1.1 200 OK\r\nContent-Length: 28\r\nConnection: close\r\n\r\n"                           \
    "event: message\ndata: hello\n\n"

/* Serves the scripted responses to sequential connections; every response must close the
 * connection so the next attempt reconnects. */
struct test_server {
    int listener_fd;
    const char *const *responses;
    size_t n_responses;
    _Atomic size_t served;
};

static void *serve_script(void *user)
{
    struct test_server *server = user;
    for (size_t i = 0; i < server->n_responses; i++) {
        struct pollfd poll_fd = {.fd = server->listener_fd, .events = POLLIN};
        if (poll(&poll_fd, 1, 10000) <= 0)
            return NULL;
        int client_fd = accept(server->listener_fd, NULL, NULL);
        if (client_fd < 0)
            return NULL;

        char request[4096];
        size_t request_len = 0;
        size_t expected_len = 0;
        while (request_len < sizeof(request) - 1) {
            ssize_t bytes_read =
                read(client_fd, request + request_len, sizeof(request) - request_len - 1);
            if (bytes_read <= 0)
                break;
            request_len += (size_t)bytes_read;
            request[request_len] = '\0';

            char *header_end = strstr(request, "\r\n\r\n");
            if (header_end && expected_len == 0)
                expected_len = (size_t)(header_end + 4 - request) + sizeof(REQUEST_BODY) - 1;
            if (expected_len > 0 && request_len >= expected_len)
                break;
        }

        const char *response = server->responses[i];
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
    if (pthread_create(thread, NULL, serve_script, server) != 0)
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

/* Fake protocol: parser lifecycle counters plus one text event per SSE data payload. */
struct fake_stream {
    stream_cb callback;
    void *callback_user;
    int inits;
    int frees;
    int finalizes;
    int live;
    int headers_built;
    int recover_grants; /* remaining 401 recoveries the hook reports as handled */
    int recover_calls;
    const char *custom_error; /* non-NULL: error_message answers 401 with a copy */
};

static char **fake_build_headers(void *ctx)
{
    struct fake_stream *fake = ctx;
    fake->headers_built++;
    char *attempt_header = xasprintf("X-Attempt: %d", fake->headers_built);
    const char *fixed[] = {attempt_header, NULL};
    char **headers = string_array_concat(fixed, NULL);
    free(attempt_header);
    return headers;
}

static void fake_parser_init(void *ctx, stream_cb callback, void *callback_user)
{
    struct fake_stream *fake = ctx;
    fake->callback = callback;
    fake->callback_user = callback_user;
    fake->inits++;
    fake->live = 1;
}

static int fake_parser_feed(const char *event_name, const char *data, void *user)
{
    (void)event_name;
    struct fake_stream *fake = user;
    struct stream_event event = {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = data}};
    fake->callback(&event, fake->callback_user);
    return 0;
}

static void fake_parser_finalize(void *ctx)
{
    struct fake_stream *fake = ctx;
    fake->finalizes++;
    struct stream_event event = {.kind = EV_DONE};
    fake->callback(&event, fake->callback_user);
}

static void fake_parser_free(void *ctx)
{
    struct fake_stream *fake = ctx;
    fake->frees++;
    fake->live = 0;
}

static int fake_recover(void *ctx, long http_status, http_tick_cb tick, void *tick_user)
{
    (void)tick;
    (void)tick_user;
    struct fake_stream *fake = ctx;
    if (http_status != 401)
        return 0;
    fake->recover_calls++;
    if (fake->recover_grants <= 0)
        return 0;
    fake->recover_grants--;
    return 1;
}

static char *fake_error_message(void *ctx, long http_status, const char *error_body)
{
    (void)error_body;
    struct fake_stream *fake = ctx;
    if (http_status != 401 || !fake->custom_error)
        return NULL;
    return xstrdup(fake->custom_error);
}

struct event_log {
    int n_text;
    int n_retry;
    int n_error;
    int n_done;
    int retry_status;
    int error_status;
    char text[128];
    char error_message[256];
};

static int log_event(const struct stream_event *event, void *user)
{
    struct event_log *log = user;
    switch (event->kind) {
    case EV_TEXT_DELTA:
        log->n_text++;
        snprintf(log->text, sizeof(log->text), "%s", event->u.text_delta.text);
        break;
    case EV_RETRY:
        log->n_retry++;
        log->retry_status = event->u.retry.http_status;
        break;
    case EV_ERROR:
        log->n_error++;
        log->error_status = event->u.error.http_status;
        snprintf(log->error_message, sizeof(log->error_message), "%s", event->u.error.message);
        break;
    case EV_DONE:
        log->n_done++;
        break;
    default:
        break;
    }
    return 0;
}

static int run_scripted(const char *const *responses, size_t n_responses, struct fake_stream *fake,
                        struct event_log *log)
{
    struct test_server server = {.responses = responses, .n_responses = n_responses};
    pthread_t thread;
    int port = start_server(&server, &thread);
    EXPECT(port > 0);
    if (port <= 0)
        return -1;

    char endpoint[64];
    snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%d/stream", port);
    struct stream_retry request = {
        .endpoint = endpoint,
        .body = REQUEST_BODY,
        .body_len = sizeof(REQUEST_BODY) - 1,
        .ctx = fake,
        .build_headers = fake_build_headers,
        .parser_init = fake_parser_init,
        .parser_feed = fake_parser_feed,
        .parser_finalize = fake_parser_finalize,
        .parser_free = fake_parser_free,
        .recover = fake_recover,
        .error_message = fake_error_message,
    };
    int result = stream_retry_run(&request, log_event, log, NULL, NULL);
    stop_server(&server, thread);
    EXPECT(atomic_load(&server.served) == n_responses);
    return result;
}

static void test_success_first_attempt(void)
{
    const char *responses[] = {SSE_OK};
    struct fake_stream fake = {0};
    struct event_log log = {0};
    int result = run_scripted(responses, 1, &fake, &log);

    EXPECT(result == 0);
    EXPECT(log.n_retry == 0);
    EXPECT(log.n_error == 0);
    EXPECT(log.n_done == 1);
    EXPECT_STR_EQ(log.text, "hello");
    EXPECT(fake.inits == 1);
    EXPECT(fake.finalizes == 1);
    EXPECT(fake.frees == 1);
    EXPECT(fake.headers_built == 1);
}

static void test_retry_then_success(void)
{
    const char *responses[] = {
        "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 4\r\nConnection: close\r\n\r\nbusy",
        SSE_OK,
    };
    struct fake_stream fake = {0};
    struct event_log log = {0};
    int result = run_scripted(responses, 2, &fake, &log);

    EXPECT(result == 0);
    EXPECT(log.n_retry == 1);
    EXPECT(log.retry_status == 503);
    EXPECT(log.n_error == 0);
    EXPECT(log.n_done == 1);
    EXPECT_STR_EQ(log.text, "hello");
    /* Fresh headers and parser state per attempt; the failed attempt's parser is freed. */
    EXPECT(fake.headers_built == 2);
    EXPECT(fake.inits == 2);
    EXPECT(fake.finalizes == 1);
    EXPECT(fake.frees == 2);
}

static void test_non_retryable_error(void)
{
    const char *responses[] = {
        "HTTP/1.1 400 Bad Request\r\nContent-Length: 35\r\nConnection: close\r\n\r\n"
        "{\"error\":{\"message\":\"bad request\"}}",
    };
    struct fake_stream fake = {0};
    struct event_log log = {0};
    int result = run_scripted(responses, 1, &fake, &log);

    EXPECT(result == 0);
    EXPECT(log.n_retry == 0);
    EXPECT(log.n_error == 1);
    EXPECT(log.error_status == 400);
    EXPECT(strstr(log.error_message, "bad request") != NULL);
    EXPECT(fake.finalizes == 0);
    EXPECT(fake.inits == 1);
    EXPECT(fake.frees == 1);
}

static void test_retry_budget_exhausted(void)
{
    const char *responses[] = {
        "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 5\r\nConnection: close\r\n\r\noops1",
        "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 5\r\nConnection: close\r\n\r\noops2",
    };
    setenv("HAX_HTTP_MAX_RETRIES", "1", 1);
    struct fake_stream fake = {0};
    struct event_log log = {0};
    int result = run_scripted(responses, 2, &fake, &log);
    unsetenv("HAX_HTTP_MAX_RETRIES");

    EXPECT(result == 0);
    EXPECT(log.n_retry == 1);
    EXPECT(log.n_error == 1);
    EXPECT(log.error_status == 500);
    EXPECT(strstr(log.error_message, "oops2") != NULL);
    EXPECT(fake.finalizes == 0);
    EXPECT(fake.inits == 2);
    EXPECT(fake.frees == 2);
}

/* A granted recovery redoes the attempt immediately: no EV_RETRY, no retry consumed. */
static void test_recover_redoes_attempt(void)
{
    const char *responses[] = {
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 5\r\nConnection: close\r\n\r\nstale",
        SSE_OK,
    };
    setenv("HAX_HTTP_MAX_RETRIES", "0", 1);
    struct fake_stream fake = {.recover_grants = 1};
    struct event_log log = {0};
    int result = run_scripted(responses, 2, &fake, &log);
    unsetenv("HAX_HTTP_MAX_RETRIES");

    EXPECT(result == 0);
    EXPECT(fake.recover_calls == 1);
    EXPECT(log.n_retry == 0);
    EXPECT(log.n_error == 0);
    EXPECT(log.n_done == 1);
    EXPECT(fake.headers_built == 2);
    EXPECT(fake.inits == 2);
    EXPECT(fake.frees == 2);
}

static void test_error_message_hook(void)
{
    const char *responses[] = {
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 5\r\nConnection: close\r\n\r\nstale",
    };
    struct fake_stream fake = {.custom_error = "token expired — run /login"};
    struct event_log log = {0};
    int result = run_scripted(responses, 1, &fake, &log);

    EXPECT(result == 0);
    EXPECT(fake.recover_calls == 1);
    EXPECT(log.n_error == 1);
    EXPECT(log.error_status == 401);
    EXPECT_STR_EQ(log.error_message, "token expired — run /login");
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    setenv("HAX_HTTP_RETRY_BASE", "1ms", 1);
    test_success_first_attempt();
    test_retry_then_success();
    test_non_retryable_error();
    test_retry_budget_exhausted();
    test_recover_redoes_attempt();
    test_error_message_hook();
    unsetenv("HAX_HTTP_RETRY_BASE");
    T_REPORT();
}
