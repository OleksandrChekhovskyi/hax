/* SPDX-License-Identifier: MIT */
#include "http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sse.h"
#include "trace.h"
#include "util.h"

#define ERR_BODY_CAP         4096
#define IDLE_TIMEOUT_DEFAULT 600L

struct curl_state {
    struct sse_parser parser;
    struct buf err_body; /* capped, for error reporting */
    http_cancel_cb cancel;
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

static size_t on_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct curl_state *s = userdata;
    size_t n = size * nmemb;

    /* Returning a short count tells libcurl the transfer failed and aborts
     * curl_easy_perform with CURLE_WRITE_ERROR. Check before parsing so we
     * don't half-feed an event into the parser. */
    if (s->cancel && s->cancel())
        return 0;

    if (s->err_body.len < ERR_BODY_CAP) {
        size_t room = ERR_BODY_CAP - s->err_body.len;
        buf_append(&s->err_body, ptr, n < room ? n : room);
    }

    sse_parser_feed(&s->parser, ptr, n);
    return n;
}

/* Periodic callback (~1Hz by default) — non-zero return aborts the transfer
 * with CURLE_ABORTED_BY_CALLBACK. Catches the case where the server is
 * silent (no chunks → on_write isn't called), e.g. local llama-server
 * during prompt eval. */
static int on_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                       curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    struct curl_state *s = clientp;
    return (s->cancel && s->cancel()) ? 1 : 0;
}

int http_sse_post(const char *url, const char *const *headers, const char *body, size_t body_len,
                  sse_cb cb, void *user, http_cancel_cb cancel, struct http_response *resp)
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
    s.cancel = cancel;

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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    /* Progress callback only when a cancel hook is provided — keeps the
     * default no-cancel path identical to before (NOPROGRESS=1). */
    if (cancel) {
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
    /* Distinguish "user cancelled" from a real transport error so the
     * agent can react cleanly without surfacing it as [error: ...]. Both
     * the write-callback short-return (CURLE_WRITE_ERROR) and the
     * progress-callback abort (CURLE_ABORTED_BY_CALLBACK) only fire when
     * cancel() returned true, so trusting the cancel hook here is safe. */
    if (rc != CURLE_OK && cancel && cancel()) {
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
    http_cancel_cb cancel;
};

static size_t http_get_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct http_get_state *s = userdata;
    size_t n = size * nmemb;
    /* Same convention as the SSE write callback: a short return aborts
     * the transfer so a cancelled probe doesn't keep buffering bytes
     * we'll throw away. */
    if (s->cancel && s->cancel())
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
    return (s->cancel && s->cancel()) ? 1 : 0;
}

int http_get(const char *url, const char *const *headers, long timeout_s, http_cancel_cb cancel,
             char **out)
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

    struct http_get_state state;
    memset(&state, 0, sizeof(state));
    state.cancel = cancel;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (hl)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_get_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    if (timeout_s > 0)
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    if (cancel) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, http_get_progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    } else {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    }

    trace_request("GET", url, headers, NULL, 0);

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
