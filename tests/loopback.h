/* SPDX-License-Identifier: MIT */
#ifndef HAX_TESTS_LOOPBACK_H
#define HAX_TESTS_LOOPBACK_H

#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "xalloc.h"

/* A loopback HTTP server for tests. One background thread serves a scripted reply to each of
 * `n_requests` sequential connections and keeps the request it read. Zero-initialize, script the
 * replies, then loopback_start (or loopback_listen and loopback_serve separately when the test
 * must act between binding and accepting). Every reply must carry "Connection: close" so the
 * client reconnects for the next one; the thread exits after the last reply, or after ten
 * seconds without a client so a scenario fails instead of hanging. */

#define LOOPBACK_MAX_REQUESTS     9
#define LOOPBACK_REQUEST_CAPACITY 8192

struct loopback {
    const char *response;                         /* reply to any request without its own */
    const char *responses[LOOPBACK_MAX_REQUESTS]; /* per-request replies, full HTTP text */
    int n_requests;                               /* connections to serve; 0 means one */
    int delay_ms;                                 /* pause between reading a request and replying */
    char requests[LOOPBACK_MAX_REQUESTS][LOOPBACK_REQUEST_CAPACITY]; /* headers and body */
    _Atomic int accepted;                                            /* connections accepted */
    _Atomic int served; /* replies fully written and closed */

    int listener_fd;
    pthread_t thread;
    int serving;
    char *owned[LOOPBACK_MAX_REQUESTS]; /* loopback_reply_ok allocations */
};

/* Read one request into `request`: the headers, then Content-Length bytes of body. */
static inline void loopback_read_request(int client_fd, char *request, size_t capacity)
{
    size_t request_len = 0;
    size_t expected_len = 0;
    while (request_len < capacity - 1) {
        ssize_t bytes_read = read(client_fd, request + request_len, capacity - request_len - 1);
        if (bytes_read <= 0)
            break;
        request_len += (size_t)bytes_read;
        request[request_len] = '\0';

        char *header_end = strstr(request, "\r\n\r\n");
        if (header_end && expected_len == 0) {
            const char *length = strstr(request, "Content-Length: ");
            expected_len =
                (size_t)(header_end + 4 - request) + (length ? strtoul(length + 16, NULL, 10) : 0);
        }
        if (expected_len > 0 && request_len >= expected_len)
            break;
    }
}

static inline void loopback_write_all(int client_fd, const char *text)
{
    size_t len = strlen(text);
    size_t written = 0;
    while (written < len) {
        ssize_t result = write(client_fd, text + written, len - written);
        if (result <= 0)
            break;
        written += (size_t)result;
    }
}

static inline void *loopback_thread(void *user)
{
    struct loopback *server = user;
    int n_requests = server->n_requests > 0 ? server->n_requests : 1;
    for (int i = 0; i < n_requests; i++) {
        struct pollfd poll_fd = {.fd = server->listener_fd, .events = POLLIN};
        if (poll(&poll_fd, 1, 10000) <= 0)
            return NULL;
        int client_fd = accept(server->listener_fd, NULL, NULL);
        if (client_fd < 0)
            return NULL;
        atomic_fetch_add(&server->accepted, 1);

        loopback_read_request(client_fd, server->requests[i], sizeof(server->requests[i]));
        if (server->delay_ms > 0) {
            struct timespec delay = {server->delay_ms / 1000, (server->delay_ms % 1000) * 1000000L};
            nanosleep(&delay, NULL);
        }
        const char *response = server->responses[i] ? server->responses[i] : server->response;
        if (response)
            loopback_write_all(client_fd, response);
        close(client_fd);
        atomic_fetch_add(&server->served, 1);
    }
    return NULL;
}

/* Bind an ephemeral loopback port without accepting yet. Returns the port, or -1. */
static inline int loopback_listen(struct loopback *server)
{
    server->listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener_fd < 0)
        return -1;

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server->listener_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->listener_fd, LOOPBACK_MAX_REQUESTS) != 0)
        goto error;

    socklen_t address_len = sizeof(address);
    if (getsockname(server->listener_fd, (struct sockaddr *)&address, &address_len) != 0)
        goto error;
    return ntohs(address.sin_port);

error:
    close(server->listener_fd);
    server->listener_fd = -1;
    return -1;
}

/* Start serving on the bound listener. Returns 0, or -1 with the listener closed. */
static inline int loopback_serve(struct loopback *server)
{
    if (pthread_create(&server->thread, NULL, loopback_thread, server) != 0) {
        close(server->listener_fd);
        server->listener_fd = -1;
        return -1;
    }
    server->serving = 1;
    return 0;
}

/* Bind and serve. Returns the port, or -1. */
static inline int loopback_start(struct loopback *server)
{
    int port = loopback_listen(server);
    if (port < 0)
        return -1;
    return loopback_serve(server) == 0 ? port : -1;
}

/* Wait for the server thread to finish, close the listener, and release scripted replies. */
static inline void loopback_stop(struct loopback *server)
{
    if (server->serving)
        pthread_join(server->thread, NULL);
    server->serving = 0;
    if (server->listener_fd >= 0)
        close(server->listener_fd);
    server->listener_fd = -1;
    for (int i = 0; i < LOOPBACK_MAX_REQUESTS; i++) {
        free(server->owned[i]);
        server->owned[i] = NULL;
    }
}

/* Script reply `index` as a 200 response carrying `body` with its Content-Length. The text is
 * owned by the server until loopback_stop. */
static inline void loopback_reply_ok(struct loopback *server, int index, const char *body)
{
    free(server->owned[index]);
    server->owned[index] =
        xasprintf("HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                  strlen(body), body);
    server->responses[index] = server->owned[index];
}

#endif /* HAX_TESTS_LOOPBACK_H */
