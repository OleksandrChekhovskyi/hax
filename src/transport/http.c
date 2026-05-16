/* SPDX-License-Identifier: MIT */
#include "transport/http.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <curl/curl.h>

#include "trace.h"
#include "util.h"
#include "transport/sse.h"

#define ERR_BODY_CAP         4096
#define IDLE_TIMEOUT_DEFAULT 600L
#define RETRY_AFTER_MAX_MS   (2L * 60L * 1000L) /* interactive cap; user can retry later */

struct curl_state {
    struct sse_parser parser;
    struct buf err_body; /* capped, for error reporting */
    http_tick_cb tick;
    void *tick_user;
    /* Borrowed handle so on_write can query CURLINFO_RESPONSE_CODE
     * before deciding whether to feed body bytes into the SSE
     * parser. See on_write for why this gate exists. */
    CURL *curl;
    long retry_after_ms;
};

struct sse_trace_wrapper {
    sse_cb inner;
    void *inner_user;
};

/* Return the idle (low-speed) timeout in seconds. Accepts plain seconds
 * or a ms/s/m/h suffix. 0 disables the guard. Unset or unparseable →
 * IDLE_TIMEOUT_DEFAULT. libcurl's CURLOPT_LOW_SPEED_TIME is whole
 * seconds, so any non-zero ms value rounds up — never time out earlier
 * than the configured duration, and sub-second values don't silently
 * floor to 0 (which would mean "disabled"). */
static long resolve_idle_timeout(void)
{
    const char *s = getenv("HAX_HTTP_IDLE_TIMEOUT");
    if (!s || !*s)
        return IDLE_TIMEOUT_DEFAULT;
    long ms = parse_duration_ms(s);
    if (ms < 0)
        return IDLE_TIMEOUT_DEFAULT;
    if (ms == 0)
        return 0;
    return ms / 1000 + (ms % 1000 ? 1 : 0);
}

static int sse_trace_cb(const char *event_name, const char *data, void *user)
{
    struct sse_trace_wrapper *w = user;
    trace_sse_event(event_name, data);
    return w->inner(event_name, data, w->inner_user);
}

static long parse_retry_after_ms(const char *value, size_t n)
{
    while (n > 0 && isspace((unsigned char)*value)) {
        value++;
        n--;
    }
    while (n > 0 && isspace((unsigned char)value[n - 1]))
        n--;
    if (n == 0)
        return 0;

    int all_digits = 1;
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)value[i])) {
            all_digits = 0;
            break;
        }
    }
    if (all_digits) {
        long secs = 0;
        for (size_t i = 0; i < n; i++) {
            int d = value[i] - '0';
            if (secs > (LONG_MAX - d) / 10)
                return RETRY_AFTER_MAX_MS;
            secs = secs * 10 + d;
        }
        if (secs > RETRY_AFTER_MAX_MS / 1000)
            return RETRY_AFTER_MAX_MS;
        return secs * 1000;
    }

    char *date = xmalloc(n + 1);
    memcpy(date, value, n);
    date[n] = '\0';
    time_t when = curl_getdate(date, NULL);
    free(date);
    if (when == (time_t)-1)
        return 0;
    time_t now = time(NULL);
    if (when <= now)
        return 0;
    if ((long)(when - now) > RETRY_AFTER_MAX_MS / 1000)
        return RETRY_AFTER_MAX_MS;
    return (long)(when - now) * 1000;
}

static size_t on_header(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct curl_state *s = userdata;
    size_t n = size * nmemb;
    static const char prefix[] = "Retry-After:";

    if (n >= sizeof(prefix) - 1 && strncasecmp(ptr, prefix, sizeof(prefix) - 1) == 0) {
        long ms = parse_retry_after_ms(ptr + sizeof(prefix) - 1, n - (sizeof(prefix) - 1));
        if (ms > 0)
            s->retry_after_ms = ms;
    }
    return n;
}

static size_t on_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct curl_state *s = userdata;
    size_t n = size * nmemb;

    /* Returning a short count tells libcurl the transfer failed and aborts
     * curl_easy_perform with CURLE_WRITE_ERROR. Check before parsing so we
     * don't half-feed an event into the parser. */
    if (s->tick && s->tick(s->tick_user))
        return 0;

    if (s->err_body.len < ERR_BODY_CAP) {
        size_t room = ERR_BODY_CAP - s->err_body.len;
        buf_append(&s->err_body, ptr, n < room ? n : room);
    }

    /* Gate the SSE parser on the HTTP response status. Some backends
     * pack errors as SSE-shaped data even on 4xx/5xx, e.g.
     *   HTTP/1.1 503 ...
     *   data: {"error":{"message":"rate limit"}}
     * Letting that through would emit EV_ERROR via the events
     * translator before the provider's retry loop has decided
     * whether to retry, polluting the agent's turn state with errors
     * from attempts we'd otherwise discard. The body bytes are still
     * captured in err_body for the eventual error surface (after
     * retries are exhausted). CURLINFO_RESPONSE_CODE is set after
     * headers and before the first body byte, so it's reliable here;
     * status==0 means we couldn't query it for some reason — fall
     * through and feed the parser, since assuming success is the
     * less-bad default than dropping a real success response. */
    long status = 0;
    curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &status);
    if (status == 0 || (status >= 200 && status < 300))
        sse_parser_feed(&s->parser, ptr, n);
    return n;
}

/* Periodic callback (~1Hz by default) — non-zero return aborts the transfer
 * with CURLE_ABORTED_BY_CALLBACK. Catches the case where the server is
 * silent (no chunks → on_write isn't called), e.g. local llama-server
 * during prompt eval. The tick may also have side effects — the agent
 * uses it to do wall-clock idle detection alongside the cancel check. */
static int on_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                       curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    struct curl_state *s = clientp;
    return (s->tick && s->tick(s->tick_user)) ? 1 : 0;
}

int http_sse_post(const char *url, const char *const *headers, const char *body, size_t body_len,
                  sse_cb cb, void *user, http_tick_cb tick, void *tick_user,
                  struct http_response *resp)
{
    memset(resp, 0, sizeof(*resp));

    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    struct curl_slist *hl = NULL;
    for (const char *const *h = headers; h && *h; h++) {
        /* On failure curl_slist_append returns NULL but the existing list is
         * unchanged — don't overwrite hl or we'd leak it. */
        struct curl_slist *next = curl_slist_append(hl, *h);
        if (!next) {
            curl_slist_free_all(hl);
            curl_easy_cleanup(curl);
            resp->error_body = xstrdup("curl_slist_append failed");
            return -1;
        }
        hl = next;
    }

    struct curl_state s;
    memset(&s, 0, sizeof(s));
    s.tick = tick;
    s.tick_user = tick_user;
    s.curl = curl;

    struct sse_trace_wrapper tw = {.inner = cb, .inner_user = user};
    if (trace_enabled())
        sse_parser_init(&s.parser, sse_trace_cb, &tw);
    else
        sse_parser_init(&s.parser, cb, user);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &s);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    /* Progress callback only when a tick hook is provided — keeps the
     * default no-tick path identical to before (NOPROGRESS=1). */
    if (tick) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &s);
    } else {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    }
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    /* Fail fast on dead connections. Streams can be legitimately long, so
     * CURLOPT_TIMEOUT is unsuitable. We rely on app-layer silence instead:
     * hosted endpoints push heartbeats or deltas regularly, but local
     * servers (notably llama-server) stay silent during prompt eval, so the
     * default is generous and HAX_HTTP_IDLE_TIMEOUT=0 disables it entirely. */
    long idle = resolve_idle_timeout();
    if (idle > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, idle);
    }

    trace_request("POST", url, headers, body, body_len);

    CURLcode rc = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    sse_parser_finalize(&s.parser);

    curl_slist_free_all(hl);
    curl_easy_cleanup(curl);

    resp->status = status;
    resp->retry_after_ms = s.retry_after_ms;
    /* Distinguish "user cancelled" from a real transport error so the
     * agent can react cleanly without surfacing it as [error: ...]. Both
     * the write-callback short-return (CURLE_WRITE_ERROR) and the
     * progress-callback abort (CURLE_ABORTED_BY_CALLBACK) only fire when
     * tick() returned non-zero, so trusting the tick hook here is safe. */
    if (rc != CURLE_OK && tick && tick(tick_user)) {
        resp->cancelled = 1;
    } else if (rc != CURLE_OK) {
        resp->error_body = xasprintf("libcurl: %s", curl_easy_strerror(rc));
    } else if (status < 200 || status >= 300) {
        resp->error_body = s.err_body.data ? buf_steal(&s.err_body) : xstrdup("(no response body)");
    }

    trace_response_status(status, resp->error_body);

    sse_parser_free(&s.parser);
    buf_free(&s.err_body);
    return (rc == CURLE_OK) ? 0 : -1;
}

struct http_get_state {
    struct buf body;
    http_tick_cb tick;
    void *tick_user;
};

static size_t http_get_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct http_get_state *s = userdata;
    size_t n = size * nmemb;
    /* Same convention as the SSE write callback: a short return aborts
     * the transfer so a cancelled probe doesn't keep buffering bytes
     * we'll throw away. */
    if (s->tick && s->tick(s->tick_user))
        return 0;
    buf_append(&s->body, ptr, n);
    return n;
}

static int http_get_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                                curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    struct http_get_state *s = clientp;
    return (s->tick && s->tick(s->tick_user)) ? 1 : 0;
}

/* Shared core for the small synchronous GET/POST probes. `method` is just
 * the label for the trace; the libcurl request shape is selected by
 * `body` (NULL = GET, non-NULL = POST with body bytes + appended
 * Content-Type: application/json). Splitting this out keeps the GET and
 * POST entry points to ~5 lines each so the only difference between
 * them is the one bit that actually differs. */
static int http_body_request(const char *method, const char *url, const char *const *headers,
                             const char *body, size_t body_len, long timeout_s, http_tick_cb tick,
                             void *tick_user, char **out)
{
    *out = NULL;

    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    struct curl_slist *hl = NULL;
    for (const char *const *h = headers; h && *h; h++) {
        struct curl_slist *next = curl_slist_append(hl, *h);
        if (!next) {
            curl_slist_free_all(hl);
            curl_easy_cleanup(curl);
            return -1;
        }
        hl = next;
    }
    if (body) {
        struct curl_slist *next = curl_slist_append(hl, "Content-Type: application/json");
        if (!next) {
            curl_slist_free_all(hl);
            curl_easy_cleanup(curl);
            return -1;
        }
        hl = next;
    }

    struct http_get_state state;
    memset(&state, 0, sizeof(state));
    state.tick = tick;
    state.tick_user = tick_user;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (hl)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);
    if (body) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_get_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    if (timeout_s > 0)
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    if (tick) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, http_get_progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    } else {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    }

    /* Mirror the auto-appended Content-Type into the trace so HAX_TRACE
     * reflects the bytes actually on the wire — otherwise a JSON POST
     * shows up in the trace with no Content-Type, which is misleading
     * when debugging server/proxy behavior that depends on it. The
     * pointer copies are cheap; allocation only on the POST path. */
    const char **trace_headers = NULL;
    if (body) {
        size_t n = 0;
        for (const char *const *h = headers; h && *h; h++)
            n++;
        trace_headers = xmalloc((n + 2) * sizeof(*trace_headers));
        for (size_t i = 0; i < n; i++)
            trace_headers[i] = headers[i];
        trace_headers[n] = "Content-Type: application/json";
        trace_headers[n + 1] = NULL;
    }
    trace_request(method, url, body ? trace_headers : headers, body, body_len);
    free(trace_headers);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(hl);
    curl_easy_cleanup(curl);

    int failed = (rc != CURLE_OK || status < 200 || status >= 300 || !state.body.data);
    /* On failure, hand whatever we received (curl error string or response
     * body) to the trace as the error body so dead probes are visible. */
    char *err = NULL;
    if (failed) {
        if (rc != CURLE_OK)
            err = xasprintf("libcurl: %s", curl_easy_strerror(rc));
        else if (state.body.data)
            err = xstrdup(state.body.data);
    }
    trace_response_status(status, err);
    free(err);

    if (failed) {
        buf_free(&state.body);
        return -1;
    }
    *out = buf_steal(&state.body);
    return 0;
}

int http_get(const char *url, const char *const *headers, long timeout_s, http_tick_cb tick,
             void *tick_user, char **out)
{
    return http_body_request("GET", url, headers, NULL, 0, timeout_s, tick, tick_user, out);
}

int http_post_json(const char *url, const char *const *headers, const char *body, size_t body_len,
                   long timeout_s, http_tick_cb tick, void *tick_user, char **out)
{
    /* Treat NULL body as a zero-length POST so the Content-Type header
     * is still attached and the request shape matches the contract. */
    return http_body_request("POST", url, headers, body ? body : "", body ? body_len : 0, timeout_s,
                             tick, tick_user, out);
}
