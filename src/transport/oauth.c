/* SPDX-License-Identifier: MIT */
#include "transport/oauth.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
/* struct timeval for SO_RCVTIMEO; not every libc leaks it through the socket headers. */
#include <sys/time.h> // IWYU pragma: keep

#include "xalloc.h"
#include "system/clock.h"
#include "system/rand.h"
#include "text/base64.h"
#include "text/sha256.h"
#include "text/url.h"
#include "transport/http.h"

/* macOS has no MSG_NOSIGNAL; SO_NOSIGPIPE on the connection covers it there. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define OAUTH_POLL_SLICE_MS 100
#define OAUTH_IO_TIMEOUT_S  2
#define OAUTH_REQUEST_CAP   8192

/* ---------- PKCE ---------- */

char *oauth_pkce_challenge(const char *verifier)
{
    unsigned char digest[SHA256_DIGEST_LEN];
    sha256(verifier, strlen(verifier), digest);
    return base64url_encode(digest, sizeof(digest), NULL);
}

void oauth_pkce_generate(char **verifier, char **challenge)
{
    unsigned char raw[64];
    random_bytes(raw, sizeof(raw));
    *verifier = base64url_encode(raw, sizeof(raw), NULL);
    *challenge = oauth_pkce_challenge(*verifier);
}

char *oauth_state_generate(void)
{
    unsigned char raw[32];
    random_bytes(raw, sizeof(raw));
    return base64url_encode(raw, sizeof(raw), NULL);
}

/* ---------- pure request parsing ---------- */

int oauth_split_request_line(const char *request, char **path_out, char **query_out)
{
    if (!request || *request == ' ')
        return -1;
    const char *target = strchr(request, ' ');
    if (!target)
        return -1;
    target++;
    if (*target != '/')
        return -1;
    size_t target_len = strcspn(target, " \r\n");
    if (target[target_len] != ' ' || strncmp(target + target_len + 1, "HTTP/", 5) != 0)
        return -1;

    const char *question = memchr(target, '?', target_len);
    size_t path_len = question ? (size_t)(question - target) : target_len;
    char *path = xmalloc(path_len + 1);
    memcpy(path, target, path_len);
    path[path_len] = '\0';

    char *query;
    if (question) {
        size_t query_len = target_len - path_len - 1;
        query = xmalloc(query_len + 1);
        memcpy(query, question + 1, query_len);
        query[query_len] = '\0';
    } else {
        query = xstrdup("");
    }

    *path_out = path;
    *query_out = query;
    return 0;
}

char *oauth_query_param(const char *query, const char *key)
{
    if (!query || !key)
        return NULL;
    size_t key_len = strlen(key);
    const char *cursor = query;
    while (*cursor) {
        size_t pair_len = strcspn(cursor, "&");
        if (pair_len >= key_len && strncmp(cursor, key, key_len) == 0) {
            if (pair_len == key_len)
                return xstrdup("");
            if (cursor[key_len] == '=')
                return url_decode(cursor + key_len + 1, pair_len - key_len - 1);
        }
        cursor += pair_len;
        if (*cursor == '&')
            cursor++;
    }
    return NULL;
}

/* ---------- loopback listener ---------- */

struct oauth_listener {
    int fds[2];
    size_t n_fds;
};

static int bind_loopback(int family, int port)
{
    int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    /* Allow rebinding through a previous login's TIME_WAIT; an active listener still refuses. */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    int bound;
    if (family == AF_INET) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bound = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    } else {
        struct sockaddr_in6 addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons((uint16_t)port);
        addr.sin6_addr = in6addr_loopback;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
        bound = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    }
    if (bound != 0 || listen(fd, 8) != 0) {
        /* Callers classify the failure (EADDRINUSE vs unsupported); close must not clobber it. */
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static int bound_port_of(int fd)
{
    struct sockaddr_storage storage;
    socklen_t storage_len = sizeof(storage);
    if (getsockname(fd, (struct sockaddr *)&storage, &storage_len) != 0)
        return -1;
    if (storage.ss_family == AF_INET) {
        struct sockaddr_in addr;
        memcpy(&addr, &storage, sizeof(addr));
        return ntohs(addr.sin_port);
    }
    struct sockaddr_in6 addr;
    memcpy(&addr, &storage, sizeof(addr));
    return ntohs(addr.sin6_port);
}

struct oauth_listener *oauth_listener_open(const int *ports, size_t n_ports, int *bound_port)
{
    /* The first pass requires both loopbacks (or IPv6 being unavailable): when another service
     * owns just [::1] on a port, a browser resolving localhost to ::1 would deliver the callback
     * to that service. The second pass settles for v4-only rather than failing when every
     * candidate carries such a conflict. */
    for (int require_v6 = 1; require_v6 >= 0; require_v6--) {
        for (size_t i = 0; i < n_ports; i++) {
            int fd4 = bind_loopback(AF_INET, ports[i]);
            if (fd4 < 0)
                continue;
            int port = bound_port_of(fd4);
            if (port <= 0) {
                close(fd4);
                continue;
            }

            int fd6 = bind_loopback(AF_INET6, port);
            if (fd6 < 0 && require_v6 && errno == EADDRINUSE) {
                close(fd4);
                continue;
            }
            struct oauth_listener *listener = xcalloc(1, sizeof(*listener));
            listener->fds[listener->n_fds++] = fd4;
            if (fd6 >= 0)
                listener->fds[listener->n_fds++] = fd6;
            if (bound_port)
                *bound_port = port;
            return listener;
        }
    }
    return NULL;
}

void oauth_listener_close(struct oauth_listener *listener)
{
    if (!listener)
        return;
    for (size_t i = 0; i < listener->n_fds; i++)
        close(listener->fds[i]);
    free(listener);
}

static void send_response(int fd, const char *status_line, const char *title, const char *message)
{
    char *html = xasprintf(
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>hax — %s</title></head>"
        "<body style=\"font-family: sans-serif; margin: 4em auto; max-width: 36em\">"
        "<h1>%s</h1><p>%s</p></body></html>",
        title, title, message);
    /* Connection: close, or the browser's kept-alive socket would occupy the listener while the
     * caller is already done with it. */
    char *response = xasprintf("HTTP/1.1 %s\r\n"
                               "Content-Type: text/html; charset=utf-8\r\n"
                               "Content-Length: %zu\r\n"
                               "Cache-Control: no-store\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "%s",
                               status_line, strlen(html), html);
    const char *cursor = response;
    size_t remaining = strlen(response);
    while (remaining > 0) {
        ssize_t count = send(fd, cursor, remaining, MSG_NOSIGNAL);
        if (count <= 0) {
            if (count < 0 && errno == EINTR)
                continue;
            break;
        }
        cursor += count;
        remaining -= (size_t)count;
    }
    free(response);
    free(html);
}

enum connection_verdict {
    CONNECTION_IGNORED, /* answered or dropped; keep listening */
    CONNECTION_CODE,
    CONNECTION_DENIED,
    CONNECTION_CANCELLED,
};

/* auth.openai.com may extend the returned state with `.onboarding_entrypoint=...` context, so a
 * `.`-separated suffix after the full expected value is accepted; the whole random state must
 * still match, preserving its CSRF entropy. */
static int state_matches(const char *expected, const char *redirect_state)
{
    if (!redirect_state)
        return 0;
    size_t expected_len = strlen(expected);
    if (strncmp(redirect_state, expected, expected_len) != 0)
        return 0;
    return redirect_state[expected_len] == '\0' || redirect_state[expected_len] == '.';
}

static enum connection_verdict handle_connection(int fd, const char *path, const char *state,
                                                 long deadline_ms, http_tick_cb tick,
                                                 void *tick_user, char **code_out,
                                                 char **detail_out)
{
    struct timeval io_timeout = {.tv_sec = OAUTH_IO_TIMEOUT_S};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    /* Read through the header terminator before answering: closing with the request unread can
     * reset the connection before the browser sees the response. Poll in short slices rather
     * than using a receive timeout, which would bound each recv, not the request — a dribbling
     * sender could pin the login past `tick` and the deadline. */
    int fd_flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fd_flags | O_NONBLOCK);
    long read_deadline_ms = monotonic_ms() + OAUTH_IO_TIMEOUT_S * 1000L;
    if (read_deadline_ms > deadline_ms)
        read_deadline_ms = deadline_ms;
    char request[OAUTH_REQUEST_CAP];
    size_t request_len = 0;
    for (;;) {
        if (tick && tick(tick_user))
            return CONNECTION_CANCELLED;
        long remaining_ms = read_deadline_ms - monotonic_ms();
        if (remaining_ms <= 0)
            return CONNECTION_IGNORED;

        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int slice_ms = remaining_ms < OAUTH_POLL_SLICE_MS ? (int)remaining_ms : OAUTH_POLL_SLICE_MS;
        int ready = poll(&pfd, 1, slice_ms);
        if (ready < 0 && errno != EINTR)
            return CONNECTION_IGNORED;
        if (ready <= 0)
            continue;

        ssize_t count = recv(fd, request + request_len, sizeof(request) - 1 - request_len, 0);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return CONNECTION_IGNORED;
        }
        if (count == 0)
            break;
        request_len += (size_t)count;
        request[request_len] = '\0';
        if (strstr(request, "\r\n\r\n") || request_len >= sizeof(request) - 1)
            break;
    }
    request[request_len] = '\0';
    /* Sends block again, bounded by the send timeout. */
    fcntl(fd, F_SETFL, fd_flags);

    char *request_path = NULL;
    char *query = NULL;
    if (oauth_split_request_line(request, &request_path, &query) != 0) {
        send_response(fd, "400 Bad Request", "Bad request",
                      "This is hax's login callback listener.");
        return CONNECTION_IGNORED;
    }

    enum connection_verdict verdict = CONNECTION_IGNORED;
    char *redirect_state = oauth_query_param(query, "state");
    char *error = oauth_query_param(query, "error");
    char *code = oauth_query_param(query, "code");
    if (strcmp(request_path, path) != 0) {
        send_response(fd, "404 Not Found", "Not found", "This is hax's login callback listener.");
    } else if (!state_matches(state, redirect_state)) {
        send_response(fd, "400 Bad Request", "Login mismatch",
                      "This redirect does not belong to the login hax is waiting for — restart "
                      "the login in hax.");
    } else if (error && *error) {
        char *description = oauth_query_param(query, "error_description");
        *detail_out =
            description && *description ? xasprintf("%s: %s", error, description) : xstrdup(error);
        free(description);
        /* Query-derived text stays out of the page so the response needs no HTML escaping. */
        send_response(fd, "200 OK", "Login failed",
                      "Return to hax for details. This tab can be closed.");
        verdict = CONNECTION_DENIED;
    } else if (code && *code) {
        *code_out = code;
        code = NULL;
        send_response(fd, "200 OK", "Login complete",
                      "You are signed in — return to hax. This tab can be closed.");
        verdict = CONNECTION_CODE;
    } else {
        send_response(fd, "400 Bad Request", "Bad request",
                      "The redirect carried neither a code nor an error.");
    }
    free(code);
    free(error);
    free(redirect_state);
    free(query);
    free(request_path);
    return verdict;
}

enum oauth_redirect_result oauth_listener_wait(struct oauth_listener *listener, const char *path,
                                               const char *state, long deadline_ms,
                                               http_tick_cb tick, void *tick_user, char **code_out,
                                               char **detail_out)
{
    *code_out = NULL;
    *detail_out = NULL;
    for (;;) {
        if (tick && tick(tick_user))
            return OAUTH_REDIRECT_CANCELLED;
        long remaining_ms = deadline_ms - monotonic_ms();
        if (remaining_ms <= 0)
            return OAUTH_REDIRECT_TIMEOUT;

        struct pollfd pfds[2];
        for (size_t i = 0; i < listener->n_fds; i++) {
            pfds[i].fd = listener->fds[i];
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }
        int slice_ms = remaining_ms < OAUTH_POLL_SLICE_MS ? (int)remaining_ms : OAUTH_POLL_SLICE_MS;
        int ready = poll(pfds, (nfds_t)listener->n_fds, slice_ms);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return OAUTH_REDIRECT_ERROR;
        }

        for (size_t i = 0; i < listener->n_fds; i++) {
            if (!(pfds[i].revents & POLLIN))
                continue;
            int conn_fd = accept(listener->fds[i], NULL, NULL);
            if (conn_fd < 0)
                continue;
            fcntl(conn_fd, F_SETFD, FD_CLOEXEC);
            enum connection_verdict verdict = handle_connection(
                conn_fd, path, state, deadline_ms, tick, tick_user, code_out, detail_out);
            close(conn_fd);
            if (verdict == CONNECTION_CODE)
                return OAUTH_REDIRECT_CODE;
            if (verdict == CONNECTION_DENIED)
                return OAUTH_REDIRECT_DENIED;
            if (verdict == CONNECTION_CANCELLED)
                return OAUTH_REDIRECT_CANCELLED;
        }
    }
}
