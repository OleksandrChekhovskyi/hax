/* SPDX-License-Identifier: MIT */
#include "loopback.h"

#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "xalloc.h"

/* Read one request into `request`: the headers, then Content-Length bytes of body. */
static void read_request(int client_fd, char *request, size_t capacity)
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

static void write_all(int client_fd, const char *text)
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

/* Bounded like accept, so a test that never releases fails instead of hanging. */
static void await_release(struct loopback *server)
{
    struct timespec tick = {0, 1000000L};
    for (int waited_ms = 0; waited_ms < 10000 && !atomic_load(&server->released); waited_ms++)
        nanosleep(&tick, NULL);
}

static void *serve_connections(void *user)
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

        read_request(client_fd, server->requests[i], sizeof(server->requests[i]));
        if (server->delay_ms > 0) {
            struct timespec delay = {server->delay_ms / 1000, (server->delay_ms % 1000) * 1000000L};
            nanosleep(&delay, NULL);
        }
        if (server->hold)
            await_release(server);
        const char *response = server->responses[i] ? server->responses[i] : server->response;
        if (response)
            write_all(client_fd, response);
        close(client_fd);
        atomic_fetch_add(&server->served, 1);
    }
    return NULL;
}

int loopback_listen(struct loopback *server)
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

int loopback_serve(struct loopback *server)
{
    if (pthread_create(&server->thread, NULL, serve_connections, server) != 0) {
        close(server->listener_fd);
        server->listener_fd = -1;
        return -1;
    }
    server->serving = 1;
    return 0;
}

int loopback_start(struct loopback *server)
{
    int port = loopback_listen(server);
    if (port < 0)
        return -1;
    return loopback_serve(server) == 0 ? port : -1;
}

void loopback_release(struct loopback *server)
{
    atomic_store(&server->released, 1);
}

void loopback_stop(struct loopback *server)
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

void loopback_reply_ok(struct loopback *server, int index, const char *body)
{
    free(server->owned[index]);
    server->owned[index] =
        xasprintf("HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                  strlen(body), body);
    server->responses[index] = server->owned[index];
}
