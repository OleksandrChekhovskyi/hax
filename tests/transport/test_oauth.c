/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "buf.h"
#include "harness.h"
#include "xalloc.h"
#include "system/clock.h"
#include "system/fd.h"
#include "text/base64.h"
#include "text/sha256.h"
#include "transport/oauth.h"

static void test_split_request_line(void)
{
    char *path = NULL;
    char *query = NULL;
    EXPECT(oauth_split_request_line("GET /auth/callback?code=abc&state=xyz HTTP/1.1\r\nHost: x\r\n",
                                    &path, &query) == 0);
    EXPECT_STR_EQ(path, "/auth/callback");
    EXPECT_STR_EQ(query, "code=abc&state=xyz");
    free(path);
    free(query);

    EXPECT(oauth_split_request_line("GET /success HTTP/1.0\r\n\r\n", &path, &query) == 0);
    EXPECT_STR_EQ(path, "/success");
    EXPECT_STR_EQ(query, "");
    free(path);
    free(query);

    EXPECT(oauth_split_request_line(NULL, &path, &query) == -1);
    EXPECT(oauth_split_request_line("", &path, &query) == -1);
    EXPECT(oauth_split_request_line(" /x HTTP/1.1", &path, &query) == -1);
    EXPECT(oauth_split_request_line("GET http://evil/ HTTP/1.1", &path, &query) == -1);
    EXPECT(oauth_split_request_line("GET /x", &path, &query) == -1);
    EXPECT(oauth_split_request_line("GET /x SPDY/1", &path, &query) == -1);
    EXPECT(oauth_split_request_line("nonsense", &path, &query) == -1);
}

static void test_query_param(void)
{
    const char *query = "a=1&code=x%2Fy+z&state=&flag&idx=9";
    char *value = oauth_query_param(query, "code");
    EXPECT_STR_EQ(value, "x/y z");
    free(value);

    value = oauth_query_param(query, "a");
    EXPECT_STR_EQ(value, "1");
    free(value);

    /* Empty values and bare keys decode to "", distinct from an absent key. */
    value = oauth_query_param(query, "state");
    EXPECT_STR_EQ(value, "");
    free(value);
    value = oauth_query_param(query, "flag");
    EXPECT_STR_EQ(value, "");
    free(value);

    /* A key must match a full pair, not a prefix of a longer key. */
    value = oauth_query_param(query, "id");
    EXPECT(value == NULL);
    EXPECT(oauth_query_param(query, "missing") == NULL);
    EXPECT(oauth_query_param(NULL, "code") == NULL);
}

static void test_pkce(void)
{
    char *verifier = NULL;
    char *challenge = NULL;
    oauth_pkce_generate(&verifier, &challenge);
    /* 64 random bytes encode to 86 base64url characters, within RFC 7636's 43..128. */
    EXPECT(strlen(verifier) == 86);
    EXPECT(strspn(verifier, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") ==
           strlen(verifier));

    unsigned char digest[SHA256_DIGEST_LEN];
    sha256(verifier, strlen(verifier), digest);
    char *want = base64url_encode(digest, sizeof(digest), NULL);
    EXPECT_STR_EQ(challenge, want);
    free(want);

    char *second_verifier = NULL;
    char *second_challenge = NULL;
    oauth_pkce_generate(&second_verifier, &second_challenge);
    EXPECT(strcmp(verifier, second_verifier) != 0);
    free(second_verifier);
    free(second_challenge);
    free(verifier);
    free(challenge);

    char *state = oauth_state_generate();
    char *second_state = oauth_state_generate();
    EXPECT(strlen(state) == 43);
    EXPECT(strcmp(state, second_state) != 0);
    free(state);
    free(second_state);
}

/* ---------- listener ---------- */

static int connect_loopback(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    EXPECT(fd >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    return fd;
}

/* Connect and queue a full request before the listener runs: the backlog holds it, so the
 * single-threaded wait below finds it on its first poll. */
static int queue_request(int port, const char *target)
{
    int fd = connect_loopback(port);
    char *request = xasprintf("GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n", target);
    EXPECT(fd_write_all(fd, request, strlen(request)) == 0);
    free(request);
    return fd;
}

static char *read_response(int fd)
{
    struct buf response;
    buf_init(&response);
    char chunk[512];
    ssize_t count;
    while ((count = read(fd, chunk, sizeof(chunk))) > 0)
        buf_append(&response, chunk, (size_t)count);
    char *text = buf_steal(&response);
    return text ? text : xstrdup("");
}

static void test_listener_captures_code(void)
{
    int port = 0;
    const int ports[] = {0};
    struct oauth_listener *listener = oauth_listener_open(ports, 1, &port);
    EXPECT(listener != NULL);
    EXPECT(port > 0);
    if (!listener)
        return;

    int fd = queue_request(port, "/auth/callback?code=abc%20def&state=S1");
    char *code = NULL;
    char *detail = NULL;
    EXPECT(oauth_listener_wait(listener, "/auth/callback", "S1", monotonic_ms() + 5000, NULL, NULL,
                               &code, &detail) == OAUTH_REDIRECT_CODE);
    EXPECT_STR_EQ(code, "abc def");
    EXPECT(detail == NULL);

    char *response = read_response(fd);
    EXPECT(strstr(response, "HTTP/1.1 200 OK\r\n") == response);
    EXPECT(strstr(response, "Connection: close") != NULL);
    EXPECT(strstr(response, "Login complete") != NULL);
    free(response);
    close(fd);
    free(code);
    oauth_listener_close(listener);
}

static void test_listener_survives_stray_requests(void)
{
    int port = 0;
    const int ports[] = {0};
    struct oauth_listener *listener = oauth_listener_open(ports, 1, &port);
    EXPECT(listener != NULL);
    if (!listener)
        return;

    /* Wrong path, wrong state, and a state-less probe of the right path must all be answered
     * without ending the wait; only the matching redirect does. */
    int stray_path = queue_request(port, "/favicon.ico");
    int stray_state = queue_request(port, "/cb?code=evil&state=WRONG");
    int stray_probe = queue_request(port, "/cb");
    int genuine = queue_request(port, "/cb?state=S2&code=ok");

    char *code = NULL;
    char *detail = NULL;
    EXPECT(oauth_listener_wait(listener, "/cb", "S2", monotonic_ms() + 5000, NULL, NULL, &code,
                               &detail) == OAUTH_REDIRECT_CODE);
    EXPECT_STR_EQ(code, "ok");
    free(code);

    char *response = read_response(stray_path);
    EXPECT(strstr(response, "404 Not Found") != NULL);
    free(response);
    response = read_response(stray_state);
    EXPECT(strstr(response, "400 Bad Request") != NULL);
    EXPECT(strstr(response, "Login mismatch") != NULL);
    free(response);
    response = read_response(stray_probe);
    EXPECT(strstr(response, "400 Bad Request") != NULL);
    free(response);
    close(stray_path);
    close(stray_state);
    close(stray_probe);
    close(genuine);
    oauth_listener_close(listener);
}

static void test_listener_reports_denial(void)
{
    int port = 0;
    const int ports[] = {0};
    struct oauth_listener *listener = oauth_listener_open(ports, 1, &port);
    EXPECT(listener != NULL);
    if (!listener)
        return;

    int fd = queue_request(port, "/cb?state=S3&error=access_denied&error_description=Nope+really");
    char *code = NULL;
    char *detail = NULL;
    EXPECT(oauth_listener_wait(listener, "/cb", "S3", monotonic_ms() + 5000, NULL, NULL, &code,
                               &detail) == OAUTH_REDIRECT_DENIED);
    EXPECT(code == NULL);
    EXPECT_STR_EQ(detail, "access_denied: Nope really");
    free(detail);

    char *response = read_response(fd);
    EXPECT(strstr(response, "Login failed") != NULL);
    free(response);
    close(fd);
    oauth_listener_close(listener);
}

/* auth.openai.com may hand the state back with onboarding context appended; the full expected
 * value plus a `.` suffix must pass, while a bare prefix or altered state must not. */
static void test_listener_accepts_state_suffix(void)
{
    int port = 0;
    const int ports[] = {0};
    struct oauth_listener *listener = oauth_listener_open(ports, 1, &port);
    EXPECT(listener != NULL);
    if (!listener)
        return;

    int truncated = queue_request(port, "/cb?code=evil&state=S");
    int extended = queue_request(port, "/cb?code=evil&state=S4x");
    int suffixed = queue_request(port, "/cb?code=ok&state=S4.onboarding_entrypoint=life_sciences");

    char *code = NULL;
    char *detail = NULL;
    EXPECT(oauth_listener_wait(listener, "/cb", "S4", monotonic_ms() + 5000, NULL, NULL, &code,
                               &detail) == OAUTH_REDIRECT_CODE);
    EXPECT_STR_EQ(code, "ok");
    free(code);

    char *response = read_response(truncated);
    EXPECT(strstr(response, "Login mismatch") != NULL);
    free(response);
    response = read_response(extended);
    EXPECT(strstr(response, "Login mismatch") != NULL);
    free(response);
    close(truncated);
    close(extended);
    close(suffixed);
    oauth_listener_close(listener);
}

static int cancel_tick(void *user)
{
    (void)user;
    return 1;
}

static int count_then_cancel_tick(void *user)
{
    int *calls = user;
    return ++*calls > 3;
}

/* A connection that never finishes its request must not pin the wait: the deadline still fires,
 * cancellation still fires mid-read, and after the stalled sender is dropped the listener still
 * serves the genuine redirect. */
static void test_listener_survives_stalled_sender(void)
{
    int port = 0;
    const int ports[] = {0};
    struct oauth_listener *listener = oauth_listener_open(ports, 1, &port);
    EXPECT(listener != NULL);
    if (!listener)
        return;

    int stalled = connect_loopback(port);
    EXPECT(fd_write_all(stalled, "GET /cb?code", 12) == 0);

    char *code = NULL;
    char *detail = NULL;
    EXPECT(oauth_listener_wait(listener, "/cb", "S6", monotonic_ms() + 300, NULL, NULL, &code,
                               &detail) == OAUTH_REDIRECT_TIMEOUT);

    /* The tick keeps being polled while the request is being read. */
    int stalled_again = connect_loopback(port);
    EXPECT(fd_write_all(stalled_again, "GET /cb?code", 12) == 0);
    int tick_calls = 0;
    EXPECT(oauth_listener_wait(listener, "/cb", "S6", monotonic_ms() + 30000,
                               count_then_cancel_tick, &tick_calls, &code,
                               &detail) == OAUTH_REDIRECT_CANCELLED);

    /* Once the stalled sender exhausts its request budget it is dropped, not served. */
    int genuine = queue_request(port, "/cb?state=S6&code=ok");
    EXPECT(oauth_listener_wait(listener, "/cb", "S6", monotonic_ms() + 30000, NULL, NULL, &code,
                               &detail) == OAUTH_REDIRECT_CODE);
    EXPECT_STR_EQ(code, "ok");
    free(code);
    close(stalled);
    close(stalled_again);
    close(genuine);
    oauth_listener_close(listener);
}

static void test_listener_timeout_and_cancel(void)
{
    int port = 0;
    const int ports[] = {0};
    struct oauth_listener *listener = oauth_listener_open(ports, 1, &port);
    EXPECT(listener != NULL);
    if (!listener)
        return;

    char *code = NULL;
    char *detail = NULL;
    EXPECT(oauth_listener_wait(listener, "/cb", "S", monotonic_ms() + 50, NULL, NULL, &code,
                               &detail) == OAUTH_REDIRECT_TIMEOUT);
    EXPECT(oauth_listener_wait(listener, "/cb", "S", monotonic_ms() + 5000, cancel_tick, NULL,
                               &code, &detail) == OAUTH_REDIRECT_CANCELLED);
    oauth_listener_close(listener);
}

/* A port whose [::1] side belongs to another service must be passed over — a browser resolving
 * localhost to ::1 would deliver the callback there — unless no candidate is fully free, where
 * v4-only coverage still beats failing. */
static void test_listener_avoids_v6_conflict(void)
{
    int squatter = socket(AF_INET6, SOCK_STREAM, 0);
    if (squatter < 0)
        T_SKIP("no IPv6 support");
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_loopback;
    if (bind(squatter, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(squatter, 1) != 0) {
        close(squatter);
        T_SKIP("cannot listen on ::1");
    }
    socklen_t addr_len = sizeof(addr);
    EXPECT(getsockname(squatter, (struct sockaddr *)&addr, &addr_len) == 0);
    int taken_port = ntohs(addr.sin6_port);

    int port = 0;
    const int ports[] = {taken_port, 0};
    struct oauth_listener *listener = oauth_listener_open(ports, 2, &port);
    EXPECT(listener != NULL);
    EXPECT(port > 0);
    EXPECT(port != taken_port);
    oauth_listener_close(listener);

    const int only_conflicted[] = {taken_port};
    listener = oauth_listener_open(only_conflicted, 1, &port);
    EXPECT(listener != NULL);
    EXPECT(port == taken_port);
    oauth_listener_close(listener);
    close(squatter);
}

static void test_listener_skips_taken_port(void)
{
    int taken_port = 0;
    const int any_port[] = {0};
    struct oauth_listener *first = oauth_listener_open(any_port, 1, &taken_port);
    EXPECT(first != NULL);
    if (!first)
        return;

    /* A port with an active listener is skipped in favor of the next candidate. */
    int port = 0;
    const int ports[] = {taken_port, 0};
    struct oauth_listener *second = oauth_listener_open(ports, 2, &port);
    EXPECT(second != NULL);
    EXPECT(port > 0);
    EXPECT(port != taken_port);
    oauth_listener_close(second);

    /* With no fallback the open fails outright. */
    const int only_taken[] = {taken_port};
    EXPECT(oauth_listener_open(only_taken, 1, &port) == NULL);
    oauth_listener_close(first);
}

int main(void)
{
    test_split_request_line();
    test_query_param();
    test_pkce();
    test_listener_captures_code();
    test_listener_survives_stray_requests();
    test_listener_accepts_state_suffix();
    test_listener_reports_denial();
    test_listener_survives_stalled_sender();
    test_listener_timeout_and_cancel();
    test_listener_avoids_v6_conflict();
    test_listener_skips_taken_port();
    T_REPORT();
}
