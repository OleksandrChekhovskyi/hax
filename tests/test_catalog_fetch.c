/* SPDX-License-Identifier: MIT */
/* End-to-end tests for the catalog's background fetch worker: a local
 * one-shot HTTP server stands in for models.dev, and each scenario runs
 * in a forked child so the module's process-lifetime latches (the
 * once-per-run fetch flag, the warm-pair tracking, the memo) start fresh
 * every time — one process can only ever exercise one fetch. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "catalog.h"
#include "harness.h"

/* Parent-made temp root; children carve their own XDG_CACHE_HOME under it. */
static char g_root[] = "/tmp/hax_test_catfetch_XXXXXX";

/* ---------------- one-shot HTTP server ---------------- */

struct srv {
    int fd;
    const char *body;
    int delay_ms;       /* pause before responding (drain-race scenarios) */
    _Atomic int served; /* response fully written — the worker has its bytes */
};

/* Serve exactly one request, then exit. The 10s accept guard keeps a
 * broken test from hanging the suite: if nothing ever connects, the
 * thread returns and the scenario's own assertions fail. */
static void *serve_once(void *arg)
{
    struct srv *s = arg;
    struct pollfd pfd = {.fd = s->fd, .events = POLLIN};
    if (poll(&pfd, 1, 10000) <= 0)
        return NULL;
    int c = accept(s->fd, NULL, NULL);
    if (c < 0)
        return NULL;
    char buf[2048];
    (void)!read(c, buf, sizeof(buf)); /* a small GET arrives in one read */
    if (s->delay_ms > 0) {
        struct timespec ts = {s->delay_ms / 1000, (s->delay_ms % 1000) * 1000000L};
        nanosleep(&ts, NULL);
    }
    dprintf(c, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
            strlen(s->body), s->body);
    close(c);
    atomic_store(&s->served, 1);
    return NULL;
}

/* Bind 127.0.0.1:ephemeral and listen; returns the port (or -1). */
static int srv_listen(struct srv *s)
{
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->fd < 0)
        return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s->fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s->fd, 1) != 0)
        return -1;
    socklen_t len = sizeof(a);
    if (getsockname(s->fd, (struct sockaddr *)&a, &len) != 0)
        return -1;
    return ntohs(a.sin_port);
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

/* Poll until (provider, model) resolves with the wanted input rate — the
 * eventually-consistent assertion for the async fetch/warm pipeline.
 * Bounded at ~3s so a regression fails rather than hangs. */
static int wait_for_rate(const char *provider, const char *model, double want)
{
    for (int i = 0; i < 300; i++) {
        struct catalog_entry e;
        if (catalog_lookup(provider, model, &e) == 0 && e.cost_input == want)
            return 1;
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* ---------------- scenarios (each runs in its own child) ---------------- */

static void scenario_cold_start(void)
{
    /* No snapshot on disk: the polls below memoize a *miss* first; the
     * fetch must land, bump the generation (invalidating that miss), and
     * make the model resolvable — the full cold-start pipeline including
     * the worker's post-fetch re-warm. */
    struct srv s = {.body = "{\"openai\": {\"models\": {"
                            "\"m1\": {\"cost\": {\"input\": 7, \"output\": 1}}}}}"};
    int port = srv_listen(&s);
    EXPECT(port > 0);
    child_env("cold", port);
    pthread_t th;
    EXPECT(pthread_create(&th, NULL, serve_once, &s) == 0);

    EXPECT(catalog_prefetch() == 0); /* no snapshot yet ⇒ nothing to be stale */
    EXPECT(wait_for_rate("openai", "m1", 7));

    pthread_join(th, NULL);
    catalog_shutdown();
}

static void scenario_refresh_invalidates_memo(void)
{
    /* A stale snapshot answers (and is memoized) first; the refresh must
     * replace the file and the generation bump must invalidate the
     * memoized old value — the "estimates self-heal when a refresh lands
     * mid-session" contract. */
    struct srv s = {.body = "{\"openai\": {\"models\": {"
                            "\"m2\": {\"cost\": {\"input\": 9, \"output\": 1}}}}}"};
    int port = srv_listen(&s);
    EXPECT(port > 0);
    child_env("refresh", port);
    write_snapshot("{\"openai\": {\"models\": {"
                   "\"m2\": {\"cost\": {\"input\": 2, \"output\": 1}}}}}");

    struct catalog_entry e;
    EXPECT(catalog_lookup("openai", "m2", &e) == 0);
    EXPECT(e.cost_input == 2); /* old snapshot, now memoized */

    pthread_t th;
    EXPECT(pthread_create(&th, NULL, serve_once, &s) == 0);
    EXPECT(catalog_prefetch() == 0); /* stale for the TTL, not for the alarm */
    EXPECT(wait_for_rate("openai", "m2", 9));

    pthread_join(th, NULL);
    catalog_shutdown();
}

/* Shared body for the two must-not-replace scenarios: a good snapshot on
 * disk, a 200 response carrying `bad_body`, and the assertion that the
 * snapshot survives. */
static void run_bad_payload_scenario(const char *name, const char *bad_body)
{
    struct srv s = {.body = bad_body};
    int port = srv_listen(&s);
    EXPECT(port > 0);
    child_env(name, port);
    write_snapshot("{\"openai\": {\"models\": {"
                   "\"m3\": {\"cost\": {\"input\": 2, \"output\": 1}}}}}");

    struct catalog_entry e;
    EXPECT(catalog_lookup("openai", "m3", &e) == 0);
    EXPECT(e.cost_input == 2);

    pthread_t th;
    EXPECT(pthread_create(&th, NULL, serve_once, &s) == 0);
    catalog_prefetch();
    /* Wait until the worker has the response bytes, then join it via
     * shutdown — validation runs to completion before the join returns,
     * so the assertion below is deterministic, not a sleep-and-hope. */
    for (int i = 0; i < 300 && !atomic_load(&s.served); i++) {
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    EXPECT(atomic_load(&s.served));
    pthread_join(th, NULL);
    catalog_shutdown(); /* also clears the memo... */

    /* ...so this re-parses whatever is on disk: still the good snapshot. */
    EXPECT(catalog_lookup("openai", "m3", &e) == 0);
    EXPECT(e.cost_input == 2);
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
    struct srv s = {.body = "{\"openai\": {\"models\": {"
                            "\"m5\": {\"cost\": {\"input\": 7, \"output\": 1}}}}}",
                    .delay_ms = 400};
    int port = srv_listen(&s);
    EXPECT(port > 0);
    child_env("drain", port);
    pthread_t th;
    EXPECT(pthread_create(&th, NULL, serve_once, &s) == 0);

    EXPECT(catalog_prefetch() == 0);
    catalog_drain(5000);
    struct catalog_entry e;
    EXPECT(catalog_lookup("openai", "m5", &e) == 0);
    EXPECT(e.cost_input == 7);

    pthread_join(th, NULL);
    catalog_shutdown();
}

static void scenario_stale_snapshot_warns(void)
{
    /* A snapshot that hasn't refreshed for over the alarm window (~30d)
     * makes prefetch report its age — the caller's cue to warn that
     * estimates may have drifted — while the refresh it spawns still
     * recovers as usual. */
    struct srv s = {.body = "{\"openai\": {\"models\": {"
                            "\"m4\": {\"cost\": {\"input\": 9, \"output\": 1}}}}}"};
    int port = srv_listen(&s);
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

    pthread_t th;
    EXPECT(pthread_create(&th, NULL, serve_once, &s) == 0);
    long stale_days = catalog_prefetch();
    EXPECT(stale_days >= 39 && stale_days <= 41);
    EXPECT(catalog_prefetch() == 0); /* one report (and one fetch) per run */
    EXPECT(wait_for_rate("openai", "m4", 9));

    pthread_join(th, NULL);
    catalog_shutdown();
}

/* ---------------- parent orchestration ---------------- */

static void run_scenario(const char *name, void (*fn)(void))
{
    pid_t pid = fork();
    if (pid == 0) {
        fn();
        _exit(t_failures ? 1 : 0);
    }
    EXPECT(pid > 0);
    if (pid <= 0)
        return;
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        FAIL("scenario '%s' failed in child (status 0x%x)", name, st);
}

int main(void)
{
    if (!mkdtemp(g_root))
        FAIL("mkdtemp: %s", strerror(errno));

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
