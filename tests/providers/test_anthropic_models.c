/* SPDX-License-Identifier: MIT */
/* Pagination of the Anthropic /models catalog, against a local server that
 * hands out canned pages. The endpoint's default page is far smaller than
 * the catalog will eventually be, so the picker follows has_more/last_id —
 * and the interesting cases are what happens when a server answers that
 * loop badly, which no live backend will reproduce on demand. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "harness.h"
#include "provider.h"
#include "providers/registry.h"

#define MAX_PAGES 4

struct srv {
    int fd;
    const char *bodies[MAX_PAGES];
    int n_bodies;
    char requests[MAX_PAGES][512]; /* request line of each served page */
    _Atomic int served;
};

/* Answer exactly n_bodies requests, one canned page each, then exit. The
 * poll guard keeps a mismatch between expected and actual request counts
 * from hanging the suite. */
static void *serve_pages(void *arg)
{
    struct srv *s = arg;
    for (int i = 0; i < s->n_bodies; i++) {
        struct pollfd pfd = {.fd = s->fd, .events = POLLIN};
        if (poll(&pfd, 1, 10000) <= 0)
            return NULL;
        int c = accept(s->fd, NULL, NULL);
        if (c < 0)
            return NULL;
        char buf[2048] = {0};
        ssize_t got = read(c, buf, sizeof(buf) - 1);
        if (got > 0) {
            char *eol = strstr(buf, "\r\n");
            size_t len = eol ? (size_t)(eol - buf) : strlen(buf);
            if (len >= sizeof(s->requests[i]))
                len = sizeof(s->requests[i]) - 1;
            memcpy(s->requests[i], buf, len);
            s->requests[i][len] = '\0';
        }
        dprintf(c, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                strlen(s->bodies[i]), s->bodies[i]);
        close(c);
        atomic_fetch_add(&s->served, 1);
    }
    return NULL;
}

static int srv_listen(struct srv *s)
{
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->fd < 0)
        return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s->fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s->fd, MAX_PAGES) != 0)
        return -1;
    socklen_t len = sizeof(a);
    if (getsockname(s->fd, (struct sockaddr *)&a, &len) != 0)
        return -1;
    return ntohs(a.sin_port);
}

/* Run the model list against a server serving `bodies` in order, filling
 * `ids` with the collected ids and *out_n with the count. Returns 0, or -1
 * when the local server couldn't be set up (the caller skips). */
static int list_against(struct srv *s, int n_bodies, char **ids, size_t max_ids, size_t *out_n)
{
    *out_n = 0;
    s->n_bodies = n_bodies;
    int port = srv_listen(s);
    if (port < 0)
        return -1;

    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    setenv("HAX_ANTHROPIC_BASE_URL", url, 1);

    pthread_t th;
    if (pthread_create(&th, NULL, serve_pages, s) != 0) {
        close(s->fd);
        return -1;
    }

    const struct provider_factory *f = provider_find("anthropic-compatible");
    EXPECT(f != NULL);
    struct provider *p = f ? f->new(f->name) : NULL;
    EXPECT(p != NULL);

    size_t n = 0;
    if (p && p->list_models) {
        struct model_info *models = NULL;
        char *err = NULL;
        int rc = p->list_models(p, &models, &n, &err, NULL, NULL);
        EXPECT(rc == 0);
        for (size_t i = 0; i < n && i < max_ids; i++)
            ids[i] = xstrdup(models[i].id);
        model_info_free(models, n);
        free(err);
    }
    if (p)
        p->destroy(p);
    pthread_join(th, NULL);
    close(s->fd);
    *out_n = n;
    return 0;
}

static void test_follows_cursor(void)
{
    /* A well-behaved two-page catalog: every page is collected once, and the
     * follow-up request carries the previous page's last_id. */
    struct srv s = {0};
    s.bodies[0] = "{\"data\":[{\"id\":\"m1\"},{\"id\":\"m2\"}],"
                  "\"has_more\":true,\"last_id\":\"m2\"}";
    s.bodies[1] = "{\"data\":[{\"id\":\"m3\"}],\"has_more\":false,\"last_id\":\"m3\"}";

    char *ids[8] = {0};
    size_t n;
    if (list_against(&s, 2, ids, 8, &n) != 0)
        T_SKIP("cannot run a loopback server here");
    EXPECT(n == 3);
    if (n == 3) {
        EXPECT_STR_EQ(ids[0], "m1");
        EXPECT_STR_EQ(ids[1], "m2");
        EXPECT_STR_EQ(ids[2], "m3");
    }
    /* A page size is always requested — the endpoint's default is small
     * enough that omitting it is how the list silently truncated before. */
    EXPECT(strstr(s.requests[0], "limit=") != NULL);
    EXPECT(strstr(s.requests[0], "after_id=") == NULL);
    EXPECT(strstr(s.requests[1], "after_id=m2") != NULL);
    for (size_t i = 0; i < n; i++)
        free(ids[i]);
}

static void test_repeated_cursor_page_is_discarded(void)
{
    /* A server whose cursor doesn't move replays the page it already sent.
     * Taking it would put every one of its rows in the picker twice, so the
     * repeat is dropped whole and paging stops — the models gathered so far
     * are still returned, since a short list beats no list. */
    struct srv s = {0};
    s.bodies[0] = "{\"data\":[{\"id\":\"m1\"},{\"id\":\"m2\"}],"
                  "\"has_more\":true,\"last_id\":\"m2\"}";
    s.bodies[1] = "{\"data\":[{\"id\":\"m1\"},{\"id\":\"m2\"}],"
                  "\"has_more\":true,\"last_id\":\"m2\"}";

    char *ids[8] = {0};
    size_t n;
    if (list_against(&s, 2, ids, 8, &n) != 0)
        T_SKIP("cannot run a loopback server here");
    EXPECT(n == 2); /* not 4 */
    if (n == 2) {
        EXPECT_STR_EQ(ids[0], "m1");
        EXPECT_STR_EQ(ids[1], "m2");
    }
    for (size_t i = 0; i < n; i++)
        free(ids[i]);
}

static void test_missing_cursor_stops(void)
{
    /* has_more with no last_id leaves nowhere to continue from; refetching
     * page one would loop forever, so stop and keep what arrived. */
    struct srv s = {0};
    s.bodies[0] = "{\"data\":[{\"id\":\"m1\"}],\"has_more\":true}";

    char *ids[8] = {0};
    size_t n;
    if (list_against(&s, 1, ids, 8, &n) != 0)
        T_SKIP("cannot run a loopback server here");
    EXPECT(n == 1);
    if (n == 1)
        EXPECT_STR_EQ(ids[0], "m1");
    for (size_t i = 0; i < n; i++)
        free(ids[i]);
}

int main(void)
{
    setenv("HAX_ANTHROPIC_API_KEY", "test-key", 1);
    test_follows_cursor();
    test_repeated_cursor_page_is_discarded();
    test_missing_cursor_stops();
    T_REPORT();
}
