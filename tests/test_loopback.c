/* SPDX-License-Identifier: MIT */
/* A raw socket client stands in for libcurl, which never splits a small request across writes. */
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "harness.h"
#include "loopback.h"

#define REPLY_A "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nConnection: close\r\n\r\nA"
#define REPLY_B "HTTP/1.1 404 Not Found\r\nContent-Length: 1\r\nConnection: close\r\n\r\nB"

static int connect_loopback(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void send_text(int fd, const char *text)
{
    size_t len = strlen(text);
    size_t written = 0;
    while (written < len) {
        ssize_t result = write(fd, text + written, len - written);
        if (result <= 0)
            break;
        written += (size_t)result;
    }
}

/* Read until the server closes the connection; a reply that never comes fails the test after
 * three seconds instead of hanging it. */
static void read_reply(int fd, char *reply, size_t capacity)
{
    size_t len = 0;
    while (len < capacity - 1) {
        struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
        if (poll(&poll_fd, 1, 3000) <= 0)
            break;
        ssize_t bytes_read = read(fd, reply + len, capacity - len - 1);
        if (bytes_read <= 0)
            break;
        len += (size_t)bytes_read;
    }
    reply[len] = '\0';
}

static void pause_briefly(void)
{
    struct timespec pause = {0, 20 * 1000000L};
    nanosleep(&pause, NULL);
}

static void test_body_split_across_writes(void)
{
    struct loopback server = {.response = REPLY_A};
    int port = loopback_start(&server);
    EXPECT(port > 0);
    if (port <= 0)
        return;
    int fd = connect_loopback(port);
    EXPECT(fd >= 0);
    send_text(fd, "POST /x HTTP/1.1\r\nHost: t\r\nContent-Length: 11\r\n\r\n");
    pause_briefly();
    send_text(fd, "hello");
    pause_briefly();
    send_text(fd, " world");
    char reply[256];
    read_reply(fd, reply, sizeof(reply));
    close(fd);
    loopback_stop(&server);

    EXPECT_STR_EQ(reply, REPLY_A);
    EXPECT(strstr(server.requests[0], "\r\n\r\nhello world") != NULL);
    EXPECT(atomic_load(&server.accepted) == 1);
    EXPECT(atomic_load(&server.served) == 1);
}

static void test_bodiless_request_answered_at_header_end(void)
{
    struct loopback server = {.response = REPLY_A};
    int port = loopback_start(&server);
    EXPECT(port > 0);
    if (port <= 0)
        return;
    int fd = connect_loopback(port);
    EXPECT(fd >= 0);
    send_text(fd, "GET /x HTTP/1.1\r\nHost: t\r\n\r\n");
    char reply[256];
    read_reply(fd, reply, sizeof(reply));
    close(fd);
    loopback_stop(&server);

    EXPECT_STR_EQ(reply, REPLY_A);
    EXPECT(strncmp(server.requests[0], "GET /x HTTP/1.1\r\n", 17) == 0);
}

static void test_scripted_replies_per_connection(void)
{
    struct loopback server = {.response = REPLY_A, .n_requests = 3};
    server.responses[1] = REPLY_B;
    loopback_reply_ok(&server, 2, "{}");
    int port = loopback_start(&server);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    static const char *const EXPECTED[] = {
        REPLY_A, REPLY_B, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}"};
    for (int i = 0; i < 3; i++) {
        int fd = connect_loopback(port);
        EXPECT(fd >= 0);
        char request[64];
        snprintf(request, sizeof(request), "GET /%d HTTP/1.1\r\nHost: t\r\n\r\n", i);
        send_text(fd, request);
        char reply[256];
        read_reply(fd, reply, sizeof(reply));
        close(fd);
        EXPECT_STR_EQ(reply, EXPECTED[i]);
    }
    loopback_stop(&server);

    EXPECT(strncmp(server.requests[0], "GET /0 ", 7) == 0);
    EXPECT(strncmp(server.requests[1], "GET /1 ", 7) == 0);
    EXPECT(strncmp(server.requests[2], "GET /2 ", 7) == 0);
    EXPECT(atomic_load(&server.served) == 3);
}

static void test_hold_replies_after_release(void)
{
    struct loopback server = {.response = REPLY_A, .hold = 1};
    int port = loopback_start(&server);
    EXPECT(port > 0);
    if (port <= 0)
        return;
    int fd = connect_loopback(port);
    EXPECT(fd >= 0);
    send_text(fd, "GET /x HTTP/1.1\r\nHost: t\r\n\r\n");
    struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
    EXPECT(poll(&poll_fd, 1, 50) == 0);

    loopback_release(&server);
    char reply[256];
    read_reply(fd, reply, sizeof(reply));
    close(fd);
    loopback_stop(&server);
    EXPECT_STR_EQ(reply, REPLY_A);
}

int main(void)
{
    test_body_split_across_writes();
    test_bodiless_request_answered_at_header_end();
    test_scripted_replies_per_connection();
    test_hold_replies_after_release();
    T_REPORT();
}
