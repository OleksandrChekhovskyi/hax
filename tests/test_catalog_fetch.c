/* SPDX-License-Identifier: MIT */
/* Each fetch scenario runs in a child because catalog_prefetch is process-wide and runs once. */
#include <errno.h>
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
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "catalog.h"
#include "harness.h"

/* Parent-made temp root; children carve their own XDG_CACHE_HOME under it. */
static char *g_root;

/* ---------------- one-shot HTTP server ---------------- */

struct test_server {
    int listen_fd;
    const char *body;
    int delay_ms;
    _Atomic int served; /* Response fully written; the fetch worker has its bytes. */
};

/* The timeout turns a missing client into a failed scenario rather than a hung test. */
static void *serve_once(void *arg)
{
    struct test_server *server = arg;
    struct pollfd poll_fd = {.fd = server->listen_fd, .events = POLLIN};
    if (poll(&poll_fd, 1, 10000) <= 0)
        return NULL;
    int client_fd = accept(server->listen_fd, NULL, NULL);
    if (client_fd < 0)
        return NULL;
    char request[2048];
    (void)!read(client_fd, request, sizeof(request));
    if (server->delay_ms > 0) {
        struct timespec delay = {server->delay_ms / 1000, (server->delay_ms % 1000) * 1000000L};
        nanosleep(&delay, NULL);
    }
    dprintf(client_fd, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
            strlen(server->body), server->body);
    close(client_fd);
    atomic_store(&server->served, 1);
    return NULL;
}

static int server_listen(struct test_server *server)
{
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0)
        return -1;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server->listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->listen_fd, 1) != 0)
        return -1;
    socklen_t address_length = sizeof(address);
    if (getsockname(server->listen_fd, (struct sockaddr *)&address, &address_length) != 0)
        return -1;
    return ntohs(address.sin_port);
}

/* ---------------- child-side helpers ---------------- */

/* Point the module at a private cache dir and the scenario's server.
 * catalog.refresh=1ms makes any existing snapshot count as stale, so the
 * fetch always spawns. */
static void child_env(const char *name, int port)
{
    char dir[512], url[64];
    snprintf(dir, sizeof(dir), "%s/%s", g_root, name);
    mkdir(dir, 0755);
    setenv("XDG_CACHE_HOME", dir, 1);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/api.json", port);
    setenv("HAX_CATALOG_URL", url, 1);
    setenv("HAX_CATALOG_REFRESH", "1ms", 1);
}

static void write_snapshot(const char *json)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/hax", getenv("XDG_CACHE_HOME"));
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/hax/catalog.json", getenv("XDG_CACHE_HOME"));
    FILE *f = fopen(path, "w");
    if (!f)
        FAIL("fopen %s: %s", path, strerror(errno));
    fputs(json, f);
    fclose(f);
}

/* Poll the asynchronous refresh for at most three seconds. */
static int wait_for_rate(const char *provider_id, const char *model, double expected_rate)
{
    for (int attempt = 0; attempt < 300; attempt++) {
        struct catalog_entry entry;
        if (catalog_lookup(NULL, provider_id, model, &entry) == 0 &&
            entry.cost_input == expected_rate)
            return 1;
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* ---------------- scenarios (each runs in its own child) ---------------- */

static void scenario_cold_start(void)
{
    /* The generation bump must invalidate the miss memoized while the first fetch runs. */
    struct test_server server = {.body = "{\"openai\": {\"models\": {"
                                         "\"m1\": {\"cost\": {\"input\": 7, \"output\": 1}}}}}"};
    int port = server_listen(&server);
    EXPECT(port > 0);
    child_env("cold", port);
    pthread_t server_thread;
    EXPECT(pthread_create(&server_thread, NULL, serve_once, &server) == 0);

    EXPECT(catalog_prefetch() == 0); /* no snapshot yet ⇒ nothing to be stale */
    EXPECT(wait_for_rate("openai", "m1", 7));

    pthread_join(server_thread, NULL);
    catalog_shutdown();
}

static void scenario_refresh_invalidates_memo(void)
{
    /* A stale snapshot answers (and is memoized) first; the refresh must
     * replace the file and the generation bump must invalidate the
     * memoized old value — the "estimates self-heal when a refresh lands
     * mid-session" contract. */
    struct test_server server = {.body = "{\"openai\": {\"models\": {"
                                         "\"m2\": {\"cost\": {\"input\": 9, \"output\": 1}}}}}"};
    int port = server_listen(&server);
    EXPECT(port > 0);
    child_env("refresh", port);
    write_snapshot("{\"openai\": {\"models\": {"
                   "\"m2\": {\"cost\": {\"input\": 2, \"output\": 1}}}}}");

    struct catalog_entry entry;
    EXPECT(catalog_lookup(NULL, "openai", "m2", &entry) == 0);
    EXPECT(entry.cost_input == 2); /* old snapshot, now memoized */

    pthread_t server_thread;
    EXPECT(pthread_create(&server_thread, NULL, serve_once, &server) == 0);
    EXPECT(catalog_prefetch() == 0); /* stale for the TTL, not for the alarm */
    EXPECT(wait_for_rate("openai", "m2", 9));

    pthread_join(server_thread, NULL);
    catalog_shutdown();
}

static void run_bad_payload_scenario(const char *name, const char *bad_body)
{
    struct test_server server = {.body = bad_body};
    int port = server_listen(&server);
    EXPECT(port > 0);
    child_env(name, port);
    write_snapshot("{\"openai\": {\"models\": {"
                   "\"m3\": {\"cost\": {\"input\": 2, \"output\": 1}}}}}");

    struct catalog_entry entry;
    EXPECT(catalog_lookup(NULL, "openai", "m3", &entry) == 0);
    EXPECT(entry.cost_input == 2);

    pthread_t server_thread;
    EXPECT(pthread_create(&server_thread, NULL, serve_once, &server) == 0);
    catalog_prefetch();
    /* catalog_shutdown joins validation after the server has delivered the full response. */
    for (int i = 0; i < 300 && !atomic_load(&server.served); i++) {
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    EXPECT(atomic_load(&server.served));
    pthread_join(server_thread, NULL);
    catalog_shutdown();

    /* Shutdown clears the memo, forcing this lookup to read the snapshot on disk. */
    EXPECT(catalog_lookup(NULL, "openai", "m3", &entry) == 0);
    EXPECT(entry.cost_input == 2);
}

static void scenario_garbage_keeps_snapshot(void)
{
    /* A 200 response that isn't JSON at all (an HTML error page behind a
     * broken proxy) must never replace a working snapshot. */
    run_bad_payload_scenario("garbage", "<html>bad gateway</html>");
}

static void scenario_json_error_keeps_snapshot(void)
{
    /* A JSON-shaped error payload parses fine but lacks the catalog shape
     * (no provider entry carrying a models object) — it must be rejected
     * too, or its fresh mtime would suppress a recovering re-fetch for a
     * whole refresh interval. */
    run_bad_payload_scenario("json-error", "{\"error\": \"rate limited\"}");
}

static void scenario_truncated_tail_keeps_snapshot(void)
{
    /* A body whose prefix validates but which is cut mid-member (a proxy
     * truncation with a happens-to-match Content-Length) must be rejected
     * whole — accepting it would silently drop every provider after the
     * cut until the next refresh. */
    run_bad_payload_scenario("truncated-tail",
                             "{\"openai\": {\"models\": {\"m3\": {}}}, \"anthropic\":");
}

static void scenario_invalid_member_keeps_snapshot(void)
{
    /* Brace-balanced garbage after a valid member: the structural scan
     * alone would wave it through, so every member slice must survive a
     * real parse before the snapshot is replaced. */
    run_bad_payload_scenario("invalid-member",
                             "{\"openai\": {\"models\": {\"m3\": {}}}, \"tail\": wat}");
}

static void scenario_trailing_garbage_keeps_snapshot(void)
{
    /* Bytes after the root object's closing brace (a concatenated or
     * corrupted response) mean the body isn't the artifact — reject. */
    run_bad_payload_scenario("trailing-garbage",
                             "{\"openai\": {\"models\": {\"m3\": {}}}} garbage");
}

static void scenario_drain_completes_fetch(void)
{
    /* The one-shot exit path drains the in-flight fetch (bounded) instead
     * of letting shutdown cancel it: with a server slower than the run, a
     * post-drain lookup must already see the fetched values — no polling,
     * and no cold cache left behind. */
    struct test_server server = {.body = "{\"openai\": {\"models\": {"
                                         "\"m5\": {\"cost\": {\"input\": 7, \"output\": 1}}}}}",
                                 .delay_ms = 400};
    int port = server_listen(&server);
    EXPECT(port > 0);
    child_env("drain", port);
    pthread_t server_thread;
    EXPECT(pthread_create(&server_thread, NULL, serve_once, &server) == 0);

    EXPECT(catalog_prefetch() == 0);
    catalog_drain(5000);
    struct catalog_entry entry;
    EXPECT(catalog_lookup(NULL, "openai", "m5", &entry) == 0);
    EXPECT(entry.cost_input == 7);

    pthread_join(server_thread, NULL);
    catalog_shutdown();
}

static void scenario_stale_snapshot_warns(void)
{
    /* A snapshot that hasn't refreshed for over the alarm window (~30d)
     * makes prefetch report its age — the caller's cue to warn that
     * estimates may have drifted — while the refresh it spawns still
     * recovers as usual. */
    struct test_server server = {.body = "{\"openai\": {\"models\": {"
                                         "\"m4\": {\"cost\": {\"input\": 9, \"output\": 1}}}}}"};
    int port = server_listen(&server);
    EXPECT(port > 0);
    child_env("stale", port);
    write_snapshot("{\"openai\": {\"models\": {"
                   "\"m4\": {\"cost\": {\"input\": 2, \"output\": 1}}}}}");
    /* Backdate the snapshot 40 days. */
    char path[600];
    snprintf(path, sizeof(path), "%s/hax/catalog.json", getenv("XDG_CACHE_HOME"));
    struct timeval tv[2] = {{time(NULL) - 40L * 24 * 60 * 60, 0},
                            {time(NULL) - 40L * 24 * 60 * 60, 0}};
    EXPECT(utimes(path, tv) == 0);

    pthread_t server_thread;
    EXPECT(pthread_create(&server_thread, NULL, serve_once, &server) == 0);
    long stale_days = catalog_prefetch();
    EXPECT(stale_days >= 39 && stale_days <= 41);
    EXPECT(catalog_prefetch() == 0); /* one report (and one fetch) per run */
    EXPECT(wait_for_rate("openai", "m4", 9));

    pthread_join(server_thread, NULL);
    catalog_shutdown();
}

/* ---------------- parent orchestration ---------------- */

static void run_scenario(const char *name, void (*scenario)(void))
{
    /* The include cleaner knows no direct glibc provider for pid_t here; its
     * typedef hides behind the ignored bits/ headers. */
    // NOLINTNEXTLINE(misc-include-cleaner)
    pid_t pid = fork();
    if (pid == 0) {
        scenario();
        _exit(t_failures ? 1 : 0);
    }
    EXPECT(pid > 0);
    if (pid <= 0)
        return;
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        FAIL("scenario '%s' failed in child (status 0x%x)", name, status);
}

int main(void)
{
    g_root = t_tempdir();

    run_scenario("cold-start", scenario_cold_start);
    run_scenario("refresh-invalidates-memo", scenario_refresh_invalidates_memo);
    run_scenario("garbage-keeps-snapshot", scenario_garbage_keeps_snapshot);
    run_scenario("json-error-keeps-snapshot", scenario_json_error_keeps_snapshot);
    run_scenario("truncated-tail-keeps-snapshot", scenario_truncated_tail_keeps_snapshot);
    run_scenario("invalid-member-keeps-snapshot", scenario_invalid_member_keeps_snapshot);
    run_scenario("trailing-garbage-keeps-snapshot", scenario_trailing_garbage_keeps_snapshot);
    run_scenario("drain-completes-fetch", scenario_drain_completes_fetch);
    run_scenario("stale-snapshot-warns", scenario_stale_snapshot_warns);

    T_REPORT();
}
