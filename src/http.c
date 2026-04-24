/* SPDX-License-Identifier: MIT */
#include "http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sse.h"
#include "util.h"

#define ERR_BODY_CAP 4096

struct curl_state {
    struct sse_parser parser;
    struct buf err_body; /* capped, for error reporting */
};

static size_t on_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct curl_state *s = userdata;
    size_t n = size * nmemb;

    if (s->err_body.len < ERR_BODY_CAP) {
        size_t room = ERR_BODY_CAP - s->err_body.len;
        buf_append(&s->err_body, ptr, n < room ? n : room);
    }

    sse_parser_feed(&s->parser, ptr, n);
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

    struct curl_state s;
    memset(&s, 0, sizeof(s));
    sse_parser_init(&s.parser, cb, user);

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

    sse_parser_finalize(&s.parser);

    curl_slist_free_all(hl);
    curl_easy_cleanup(curl);

    resp->status = status;
    if (rc != CURLE_OK) {
        resp->error_body = xasprintf("libcurl: %s", curl_easy_strerror(rc));
    } else if (status < 200 || status >= 300) {
        resp->error_body = s.err_body.data ? buf_steal(&s.err_body) : xstrdup("(no response body)");
    }

    sse_parser_free(&s.parser);
    buf_free(&s.err_body);
    return (rc == CURLE_OK) ? 0 : -1;
}
