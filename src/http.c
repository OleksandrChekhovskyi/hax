/* SPDX-License-Identifier: MIT */
#include "http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define ERR_BODY_CAP 4096

struct sse_state {
    struct buf line;  /* current partial line */
    struct buf event; /* current event's `event:` value */
    struct buf data;  /* current event's `data:` payload */

    sse_cb cb;
    void *user;
    int cb_abort;

    struct buf err_body; /* capped, for error reporting */
};

static void emit_event(struct sse_state *s)
{
    if (s->event.len == 0 && s->data.len == 0)
        return;
    if (!s->cb_abort) {
        const char *ev = s->event.data ? s->event.data : "";
        const char *dt = s->data.data ? s->data.data : "";
        if (s->cb(ev, dt, s->user) != 0)
            s->cb_abort = 1;
    }
    buf_reset(&s->event);
    buf_reset(&s->data);
}

static void process_line(struct sse_state *s, const char *line, size_t n)
{
    if (n && line[n - 1] == '\r')
        n--;

    if (n == 0) {
        emit_event(s);
        return;
    }
    if (line[0] == ':')
        return; /* comment */

    const char *colon = memchr(line, ':', n);
    const char *field = line;
    size_t field_len = colon ? (size_t)(colon - line) : n;
    const char *value = colon ? colon + 1 : "";
    size_t value_len = colon ? n - field_len - 1 : 0;
    if (value_len && value[0] == ' ') {
        value++;
        value_len--;
    }

    if (field_len == 5 && memcmp(field, "event", 5) == 0) {
        buf_reset(&s->event);
        buf_append(&s->event, value, value_len);
    } else if (field_len == 4 && memcmp(field, "data", 4) == 0) {
        if (s->data.len > 0)
            buf_append(&s->data, "\n", 1);
        buf_append(&s->data, value, value_len);
    }
    /* ignore id:, retry:, and unknown fields */
}

static size_t on_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct sse_state *s = userdata;
    size_t n = size * nmemb;

    if (s->err_body.len < ERR_BODY_CAP) {
        size_t room = ERR_BODY_CAP - s->err_body.len;
        buf_append(&s->err_body, ptr, n < room ? n : room);
    }

    size_t i = 0;
    for (size_t j = 0; j < n; j++) {
        if (ptr[j] == '\n') {
            buf_append(&s->line, ptr + i, j - i);
            process_line(s, s->line.data ? s->line.data : "", s->line.len);
            buf_reset(&s->line);
            i = j + 1;
        }
    }
    if (i < n)
        buf_append(&s->line, ptr + i, n - i);

    return n;
}

int http_sse_post(const char *url, const char *const *headers, const char *body, size_t body_len,
                  sse_cb cb, void *user, struct http_response *resp)
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

    struct sse_state s;
    memset(&s, 0, sizeof(s));
    s.cb = cb;
    s.user = user;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    /* Fail fast on dead connections. Streams can be legitimately long, so
     * CURLOPT_TIMEOUT is unsuitable — but the backend should always push
     * *something* (heartbeats, deltas) within a minute. */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);

    CURLcode rc = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    /* Flush trailing line / pending event */
    if (s.line.len)
        process_line(&s, s.line.data, s.line.len);
    if (s.event.len || s.data.len)
        emit_event(&s);

    curl_slist_free_all(hl);
    curl_easy_cleanup(curl);

    resp->status = status;
    if (rc != CURLE_OK) {
        resp->error_body = xasprintf("libcurl: %s", curl_easy_strerror(rc));
    } else if (status < 200 || status >= 300) {
        resp->error_body = s.err_body.data ? buf_steal(&s.err_body) : xstrdup("(no response body)");
    }

    buf_free(&s.line);
    buf_free(&s.event);
    buf_free(&s.data);
    buf_free(&s.err_body);
    return (rc == CURLE_OK) ? 0 : -1;
}
