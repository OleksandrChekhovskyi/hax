/* SPDX-License-Identifier: MIT */
#include "transport/http.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/system.h>
#include <curl/typecheck-gcc.h>

#include "buf.h"
#include "trace.h"
#include "version.h"
#include "xalloc.h"
#include "transport/ca.h"
#include "transport/sse.h"

#define ERROR_BODY_MAX_BYTES 4096
/* Default identity; a User-Agent entry in the caller's headers overrides it. */
#define HTTP_USER_AGENT "hax/" HAX_VERSION
/* Bound server-requested waits so an interactive request remains responsive. */
#define RETRY_AFTER_MAX_MS (2L * 60L * 1000L)

struct transfer_tick {
    http_tick_cb callback;
    void *user;
    int cancelled;
};

struct sse_request_state {
    struct sse_parser parser;
    struct buf error_body;
    struct transfer_tick tick;
    CURL *curl; /* borrowed; needed to classify body bytes by response status */
    long retry_after_ms;
};

struct buffered_request_state {
    struct buf body;
    long max_bytes;
    struct transfer_tick tick;
};

struct traced_sse_callback {
    sse_cb callback;
    void *user;
};

static char *curl_error_body(CURLcode result)
{
    const char *hint = ca_verify_hint(result);

    if (hint)
        return xasprintf("libcurl: %s (%s)", curl_easy_strerror(result), hint);
    return xasprintf("libcurl: %s", curl_easy_strerror(result));
}

static int poll_transfer(struct transfer_tick *tick)
{
    if (!tick->callback || !tick->callback(tick->user))
        return 0;

    tick->cancelled = 1;
    return 1;
}

static int trace_sse_callback(const char *event_name, const char *data, void *user)
{
    struct traced_sse_callback *traced = user;

    trace_sse_event(event_name, data);
    return traced->callback(event_name, data, traced->user);
}

static int build_header_list(const char *const *headers, const char *extra_header,
                             struct curl_slist **list_out)
{
    struct curl_slist *list = NULL;

    for (const char *const *header = headers; header && *header; header++) {
        struct curl_slist *next = curl_slist_append(list, *header);
        if (!next)
            goto error;
        list = next;
    }
    if (extra_header) {
        struct curl_slist *next = curl_slist_append(list, extra_header);
        if (!next)
            goto error;
        list = next;
    }

    *list_out = list;
    return 0;

error:
    curl_slist_free_all(list);
    *list_out = NULL;
    return -1;
}

static const char **copy_headers_with(const char *const *headers, const char *extra_header)
{
    size_t count = 0;

    for (const char *const *header = headers; header && *header; header++)
        count++;

    const char **copy = xmalloc((count + 2) * sizeof(*copy));
    for (size_t i = 0; i < count; i++)
        copy[i] = headers[i];
    copy[count] = extra_header;
    copy[count + 1] = NULL;
    return copy;
}

static long parse_retry_after_ms(const char *value, size_t len)
{
    while (len > 0 && isspace((unsigned char)*value)) {
        value++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)value[len - 1]))
        len--;
    if (len == 0)
        return 0;

    int all_digits = 1;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)value[i])) {
            all_digits = 0;
            break;
        }
    }
    if (all_digits) {
        long seconds = 0;
        for (size_t i = 0; i < len; i++) {
            int digit = value[i] - '0';
            if (seconds > (LONG_MAX - digit) / 10)
                return RETRY_AFTER_MAX_MS;
            seconds = seconds * 10 + digit;
        }
        if (seconds > RETRY_AFTER_MAX_MS / 1000)
            return RETRY_AFTER_MAX_MS;
        return seconds * 1000;
    }

    char *date = xmalloc(len + 1);
    memcpy(date, value, len);
    date[len] = '\0';
    time_t retry_at = curl_getdate(date, NULL);
    free(date);
    if (retry_at == (time_t)-1)
        return 0;

    time_t now = time(NULL);
    if (retry_at <= now)
        return 0;
    if ((long)(retry_at - now) > RETRY_AFTER_MAX_MS / 1000)
        return RETRY_AFTER_MAX_MS;
    return (long)(retry_at - now) * 1000;
}

static size_t capture_sse_header(char *data, size_t size, size_t count, void *user)
{
    static const char retry_after[] = "Retry-After:";
    struct sse_request_state *state = user;
    size_t len = size * count;

    if (len >= sizeof(retry_after) - 1 &&
        strncasecmp(data, retry_after, sizeof(retry_after) - 1) == 0) {
        long delay_ms =
            parse_retry_after_ms(data + sizeof(retry_after) - 1, len - (sizeof(retry_after) - 1));
        if (delay_ms > 0)
            state->retry_after_ms = delay_ms;
    }
    return len;
}

static size_t receive_sse_data(char *data, size_t size, size_t count, void *user)
{
    struct sse_request_state *state = user;
    size_t len = size * count;

    /* A short write aborts before a partial event reaches the parser. */
    if (poll_transfer(&state->tick))
        return 0;

    if (state->error_body.len < ERROR_BODY_MAX_BYTES) {
        size_t available = ERROR_BODY_MAX_BYTES - state->error_body.len;
        buf_append(&state->error_body, data, len < available ? len : available);
    }

    /* Error responses can contain valid SSE framing. Delivering those events would mutate the
     * caller's stream state before its retry policy classifies the response. */
    long status = 0;
    curl_easy_getinfo(state->curl, CURLINFO_RESPONSE_CODE, &status);
    if (status == 0 || (status >= 200 && status < 300))
        sse_parser_feed(&state->parser, data, len);
    return len;
}

static int poll_sse_transfer(void *user, curl_off_t download_total, curl_off_t downloaded,
                             curl_off_t upload_total, curl_off_t uploaded)
{
    (void)download_total;
    (void)downloaded;
    (void)upload_total;
    (void)uploaded;
    struct sse_request_state *state = user;
    return poll_transfer(&state->tick);
}

int http_sse_post(const char *url, const char *const *headers, const char *body, size_t body_len,
                  long idle_timeout_s, sse_cb callback, void *user, http_tick_cb tick,
                  void *tick_user, struct http_response *response)
{
    memset(response, 0, sizeof(*response));

    CURL *curl = curl_easy_init();
    if (!curl) {
        response->error_body = xstrdup("curl_easy_init failed");
        return -1;
    }

    struct curl_slist *header_list = NULL;
    if (build_header_list(headers, NULL, &header_list) != 0) {
        curl_easy_cleanup(curl);
        response->error_body = xstrdup("curl_slist_append failed");
        return -1;
    }

    struct sse_request_state state = {
        .tick = {.callback = tick, .user = tick_user},
        .curl = curl,
    };
    struct traced_sse_callback traced = {.callback = callback, .user = user};
    if (trace_enabled())
        sse_parser_init(&state.parser, trace_sse_callback, &traced);
    else
        sse_parser_init(&state.parser, callback, user);

    ca_apply(curl);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, HTTP_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, capture_sse_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_sse_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    if (tick) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, poll_sse_transfer);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    } else {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    }
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    /* A total timeout would reject healthy long streams. The low-speed timeout detects dead
     * connections while allowing local servers to stay quiet during prompt evaluation. */
    if (idle_timeout_s > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, idle_timeout_s);
    }

    trace_request("POST", url, headers, body, body_len);
    CURLcode result = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (!state.tick.cancelled)
        sse_parser_finalize(&state.parser);

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    response->status = status;
    response->retry_after_ms = state.retry_after_ms;
    if (state.tick.cancelled) {
        response->cancelled = 1;
    } else if (result != CURLE_OK) {
        response->error_body = curl_error_body(result);
    } else if (status < 200 || status >= 300) {
        response->error_body =
            state.error_body.data ? buf_steal(&state.error_body) : xstrdup("(no response body)");
    }

    trace_response_status(status, response->error_body);
    sse_parser_free(&state.parser);
    buf_free(&state.error_body);
    return result == CURLE_OK ? 0 : -1;
}

static size_t receive_buffered_data(char *data, size_t size, size_t count, void *user)
{
    struct buffered_request_state *state = user;
    size_t len = size * count;

    if (poll_transfer(&state->tick))
        return 0;
    if (state->max_bytes > 0 && (state->body.len > (size_t)state->max_bytes ||
                                 len > (size_t)state->max_bytes - state->body.len))
        return 0;

    buf_append(&state->body, data, len);
    return len;
}

static int poll_buffered_transfer(void *user, curl_off_t download_total, curl_off_t downloaded,
                                  curl_off_t upload_total, curl_off_t uploaded)
{
    (void)download_total;
    (void)downloaded;
    (void)upload_total;
    (void)uploaded;
    struct buffered_request_state *state = user;
    return poll_transfer(&state->tick);
}

/* `content_type` is a complete "Content-Type: ..." header line or NULL. With `any_status` set, a
 * completed transfer succeeds regardless of HTTP status and `*out` may be NULL for an empty body;
 * otherwise success requires a non-empty 2xx response. */
static int buffered_request(const char *url, const char *const *headers, const char *content_type,
                            const char *body, size_t body_len, long timeout_s, long max_bytes,
                            http_tick_cb tick, void *tick_user, int any_status, char **out,
                            long *status_out)
{
    *out = NULL;
    if (status_out)
        *status_out = 0;

    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    struct curl_slist *header_list = NULL;
    if (build_header_list(headers, content_type, &header_list) != 0) {
        curl_easy_cleanup(curl);
        return -1;
    }

    struct buffered_request_state state = {
        .max_bytes = max_bytes,
        .tick = {.callback = tick, .user = tick_user},
    };

    ca_apply(curl);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, HTTP_USER_AGENT);
    if (header_list)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    if (body) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_buffered_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    if (timeout_s > 0)
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    if (tick) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, poll_buffered_transfer);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    } else {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    }

    const char **trace_headers = body ? copy_headers_with(headers, content_type) : NULL;
    trace_request(body ? "POST" : "GET", url, body ? trace_headers : headers, body, body_len);
    free(trace_headers);

    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status_out)
        *status_out = status;

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    int http_failed = status < 200 || status >= 300;
    int failed = result != CURLE_OK || (!any_status && (http_failed || !state.body.data));
    char *error_body = NULL;
    if (result != CURLE_OK) {
        /* Buffered callers only see -1; surface the certificate advice out of band. */
        ca_warn_verify_failure(result);
        error_body = curl_error_body(result);
    } else if (http_failed && state.body.data) {
        error_body = xstrdup(state.body.data);
    }
    trace_response_status(status, error_body);
    free(error_body);

    if (failed) {
        buf_free(&state.body);
        return -1;
    }
    *out = state.body.data ? buf_steal(&state.body) : NULL;
    buf_free(&state.body);
    return 0;
}

int http_get(const char *url, const char *const *headers, long timeout_s, long max_bytes,
             http_tick_cb tick, void *tick_user, char **out, long *status_out)
{
    return buffered_request(url, headers, NULL, NULL, 0, timeout_s, max_bytes, tick, tick_user, 0,
                            out, status_out);
}

int http_post_json(const char *url, const char *const *headers, const char *body, size_t body_len,
                   long timeout_s, long max_bytes, http_tick_cb tick, void *tick_user, char **out)
{
    return buffered_request(url, headers, "Content-Type: application/json", body ? body : "",
                            body ? body_len : 0, timeout_s, max_bytes, tick, tick_user, 0, out,
                            NULL);
}

int http_post(const char *url, const char *const *headers, const char *content_type,
              const char *body, size_t body_len, long timeout_s, long max_bytes, http_tick_cb tick,
              void *tick_user, char **out, long *status_out)
{
    char *content_type_header = content_type ? xasprintf("Content-Type: %s", content_type) : NULL;
    long status = 0;
    int result =
        buffered_request(url, headers, content_type_header, body ? body : "", body ? body_len : 0,
                         timeout_s, max_bytes, tick, tick_user, 1, out, &status);
    free(content_type_header);
    if (status_out)
        *status_out = result == 0 ? status : 0;
    return result;
}
