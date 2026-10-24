/* SPDX-License-Identifier: MIT */
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "loopback.h"
#include "provider.h"
#include "xalloc.h"
#include "providers/stream_retry.h"
#include "transport/http.h"

#define REQUEST_BODY "{\"model\":\"m\"}"

#define SSE_OK                                                                                     \
    "HTTP/1.1 200 OK\r\nContent-Length: 28\r\nConnection: close\r\n\r\n"                           \
    "event: message\ndata: hello\n\n"

/* Like SSE_OK plus the payload a completion-tracking fake accepts as terminal. */
#define SSE_TERMINAL                                                                               \
    "HTTP/1.1 200 OK\r\nContent-Length: 40\r\nConnection: close\r\n\r\n"                           \
    "event: message\ndata: hello\n\ndata: done\n\n"

/* The body falls short of its declared length: a 2xx stream cut by a transport error. */
#define SSE_TRUNCATED                                                                              \
    "HTTP/1.1 200 OK\r\nContent-Length: 999\r\nConnection: close\r\n\r\n"                          \
    "event: message\ndata: hello\n\n"

/* The terminal payload arrives, then the framing falls short of the declared length. */
#define SSE_TERMINAL_TRUNCATED                                                                     \
    "HTTP/1.1 200 OK\r\nContent-Length: 999\r\nConnection: close\r\n\r\n"                          \
    "event: message\ndata: hello\n\ndata: done\n\n"

/* Fake protocol: parser lifecycle counters plus one text event per SSE data payload. With
 * track_completion set, only a "done" payload marks the stream terminal, and captured usage
 * is exposed for EV_RETRY. */
struct fake_stream {
    stream_cb callback;
    void *callback_user;
    int inits;
    int frees;
    int finalizes;
    int live;
    int headers_built;
    int recover_grants; /* remaining 401 recoveries the hook reports as handled */
    int recover_calls;
    const char *custom_error; /* non-NULL: error_message answers 401 with a copy */
    int track_completion;
    int complete;
    struct stream_usage usage;
};

static char **fake_build_headers(void *ctx)
{
    struct fake_stream *fake = ctx;
    fake->headers_built++;
    char *attempt_header = xasprintf("X-Attempt: %d", fake->headers_built);
    const char *fixed[] = {attempt_header, NULL};
    char **headers = string_array_concat(fixed, NULL);
    free(attempt_header);
    return headers;
}

static void fake_parser_init(void *ctx, stream_cb callback, void *callback_user)
{
    struct fake_stream *fake = ctx;
    fake->callback = callback;
    fake->callback_user = callback_user;
    fake->inits++;
    fake->live = 1;
    fake->complete = 0;
}

static int fake_parser_feed(const char *event_name, const char *data, void *user)
{
    (void)event_name;
    struct fake_stream *fake = user;
    if (strcmp(data, "done") == 0) {
        fake->complete = 1;
        return 0;
    }
    struct stream_event event = {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = data}};
    fake->callback(&event, fake->callback_user);
    return 0;
}

static int fake_parser_complete(void *ctx)
{
    struct fake_stream *fake = ctx;
    return !fake->track_completion || fake->complete;
}

static const struct stream_usage *fake_parser_usage(void *ctx)
{
    struct fake_stream *fake = ctx;
    return fake->track_completion ? &fake->usage : NULL;
}

static void fake_parser_finalize(void *ctx)
{
    struct fake_stream *fake = ctx;
    fake->finalizes++;
    struct stream_event event = {.kind = EV_DONE};
    fake->callback(&event, fake->callback_user);
}

static void fake_parser_free(void *ctx)
{
    struct fake_stream *fake = ctx;
    fake->frees++;
    fake->live = 0;
}

static int fake_recover(void *ctx, long http_status, http_tick_cb tick, void *tick_user)
{
    (void)tick;
    (void)tick_user;
    struct fake_stream *fake = ctx;
    if (http_status != 401)
        return 0;
    fake->recover_calls++;
    if (fake->recover_grants <= 0)
        return 0;
    fake->recover_grants--;
    return 1;
}

static char *fake_error_message(void *ctx, long http_status, const char *error_body)
{
    (void)error_body;
    struct fake_stream *fake = ctx;
    if (http_status != 401 || !fake->custom_error)
        return NULL;
    return xstrdup(fake->custom_error);
}

struct event_log {
    int n_text;
    int n_retry;
    int n_error;
    int n_done;
    int retry_status;
    long retry_usage_input;
    long error_usage_input;
    int error_status;
    enum stream_error_kind error_kind;
    char text[128];
    char error_message[256];
};

static int log_event(const struct stream_event *event, void *user)
{
    struct event_log *log = user;
    switch (event->kind) {
    case EV_TEXT_DELTA:
        log->n_text++;
        snprintf(log->text, sizeof(log->text), "%s", event->u.text_delta.text);
        break;
    case EV_RETRY:
        log->n_retry++;
        log->retry_status = event->u.retry.http_status;
        if (event->u.retry.usage && event->u.retry.usage->input_tokens > 0)
            log->retry_usage_input += event->u.retry.usage->input_tokens;
        break;
    case EV_ERROR:
        log->n_error++;
        log->error_status = event->u.error.http_status;
        log->error_kind = event->u.error.error_kind;
        if (event->u.error.usage)
            log->error_usage_input = event->u.error.usage->input_tokens;
        snprintf(log->error_message, sizeof(log->error_message), "%s", event->u.error.message);
        break;
    case EV_DONE:
        log->n_done++;
        break;
    default:
        break;
    }
    return 0;
}

static int run_scripted(const char *const *responses, size_t n_responses, struct fake_stream *fake,
                        struct event_log *log)
{
    EXPECT(n_responses <= LOOPBACK_MAX_REQUESTS);
    struct loopback server = {.n_requests = (int)n_responses};
    for (size_t i = 0; i < n_responses && i < LOOPBACK_MAX_REQUESTS; i++)
        server.responses[i] = responses[i];
    int port = loopback_start(&server);
    EXPECT(port > 0);
    if (port <= 0)
        return -1;

    char endpoint[64];
    snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%d/stream", port);
    struct stream_retry request = {
        .endpoint = endpoint,
        .body = REQUEST_BODY,
        .body_len = sizeof(REQUEST_BODY) - 1,
        .ctx = fake,
        .build_headers = fake_build_headers,
        .parser_init = fake_parser_init,
        .parser_feed = fake_parser_feed,
        .parser_finalize = fake_parser_finalize,
        .parser_free = fake_parser_free,
        .parser_complete = fake_parser_complete,
        .parser_usage = fake_parser_usage,
        .recover = fake_recover,
        .error_message = fake_error_message,
    };
    int result = stream_retry_run(&request, log_event, log, NULL, NULL);
    loopback_stop(&server);
    EXPECT(atomic_load(&server.served) == (int)n_responses);
    return result;
}

static void test_success_first_attempt(void)
{
    const char *responses[] = {SSE_OK};
    struct fake_stream fake = {0};
    struct event_log log = {0};
    int result = run_scripted(responses, 1, &fake, &log);

    EXPECT(result == 0);
    EXPECT(log.n_retry == 0);
    EXPECT(log.n_error == 0);
    EXPECT(log.n_done == 1);
    EXPECT_STR_EQ(log.text, "hello");
    EXPECT(fake.inits == 1);
    EXPECT(fake.finalizes == 1);
    EXPECT(fake.frees == 1);
    EXPECT(fake.headers_built == 1);
}

static void test_retry_then_success(void)
{
    const char *responses[] = {
        "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 4\r\nConnection: close\r\n\r\nbusy",
        SSE_OK,
    };
    struct fake_stream fake = {0};
    struct event_log log = {0};
    int result = run_scripted(responses, 2, &fake, &log);

    EXPECT(result == 0);
    EXPECT(log.n_retry == 1);
    EXPECT(log.retry_status == 503);
    EXPECT(log.n_error == 0);
    EXPECT(log.n_done == 1);
    EXPECT_STR_EQ(log.text, "hello");
    /* Fresh headers and parser state per attempt; the failed attempt's parser is freed. */
    EXPECT(fake.headers_built == 2);
    EXPECT(fake.inits == 2);
    EXPECT(fake.finalizes == 1);
    EXPECT(fake.frees == 2);
}

static void test_overflow_refusal_is_classified(void)
{
    const char *responses[] = {
        "HTTP/1.1 400 Bad Request\r\nContent-Length: 58\r\nConnection: close\r\n\r\n"
        "{\"error\":{\"message\":\"x\",\"code\":\"context_length_exceeded\"}}",
    };
    struct fake_stream fake = {0};
    struct event_log log = {0};
    run_scripted(responses, 1, &fake, &log);

    EXPECT(log.n_retry == 0);
    EXPECT(log.n_error == 1);
    EXPECT(log.error_kind == STREAM_ERROR_CONTEXT_OVERFLOW);
}

static void test_non_retryable_error(void)
{
    const char *responses[] = {
        "HTTP/1.1 400 Bad Request\r\nContent-Length: 35\r\nConnection: close\r\n\r\n"
        "{\"error\":{\"message\":\"bad request\"}}",
    };
    struct fake_stream fake = {0};
    struct event_log log = {0};
    int result = run_scripted(responses, 1, &fake, &log);

    EXPECT(result == 0);
    EXPECT(log.n_retry == 0);
    EXPECT(log.n_error == 1);
    EXPECT(log.error_status == 400);
    EXPECT(strstr(log.error_message, "bad request") != NULL);
    EXPECT(log.error_kind == STREAM_ERROR_OTHER);
    EXPECT(fake.finalizes == 0);
    EXPECT(fake.inits == 1);
    EXPECT(fake.frees == 1);
}

static void test_retry_budget_exhausted(void)
{
    const char *responses[] = {
        "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 5\r\nConnection: close\r\n\r\noops1",
        "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 5\r\nConnection: close\r\n\r\noops2",
    };
    setenv("HAX_HTTP_MAX_RETRIES", "1", 1);
    struct fake_stream fake = {0};
    struct event_log log = {0};
    int result = run_scripted(responses, 2, &fake, &log);
    unsetenv("HAX_HTTP_MAX_RETRIES");

    EXPECT(result == 0);
    EXPECT(log.n_retry == 1);
    EXPECT(log.n_error == 1);
    EXPECT(log.error_status == 500);
    EXPECT(strstr(log.error_message, "oops2") != NULL);
    EXPECT(fake.finalizes == 0);
    EXPECT(fake.inits == 2);
    EXPECT(fake.frees == 2);
}

/* A granted recovery redoes the attempt immediately: no EV_RETRY, no retry consumed. */
static void test_recover_redoes_attempt(void)
{
    const char *responses[] = {
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 5\r\nConnection: close\r\n\r\nstale",
        SSE_OK,
    };
    setenv("HAX_HTTP_MAX_RETRIES", "0", 1);
    struct fake_stream fake = {.recover_grants = 1};
    struct event_log log = {0};
    int result = run_scripted(responses, 2, &fake, &log);
    unsetenv("HAX_HTTP_MAX_RETRIES");

    EXPECT(result == 0);
    EXPECT(fake.recover_calls == 1);
    EXPECT(log.n_retry == 0);
    EXPECT(log.n_error == 0);
    EXPECT(log.n_done == 1);
    EXPECT(fake.headers_built == 2);
    EXPECT(fake.inits == 2);
    EXPECT(fake.frees == 2);
}

/* A completion-tracking fake whose parser reports `input_tokens` (nothing when negative). */
static struct fake_stream fake_tracking(long input_tokens)
{
    struct fake_stream fake = {.track_completion = 1};
    fake.usage = (struct stream_usage){
        .input_tokens = input_tokens,
        .output_tokens = -1,
        .cached_tokens = -1,
        .cache_write_tokens = -1,
        .cache_write_1h_tokens = -1,
        .cost = -1,
    };
    return fake;
}

/* A 2xx stream that closes without a terminal state died mid-generation: retried like a
 * transient failure, with the dead attempt's usage carried on EV_RETRY. */
static void test_midstream_death_retried(void)
{
    const char *responses[] = {SSE_OK, SSE_TERMINAL};
    struct fake_stream fake = fake_tracking(7);
    struct event_log log = {0};
    int result = run_scripted(responses, 2, &fake, &log);

    EXPECT(result == 0);
    EXPECT(log.n_retry == 1);
    EXPECT(log.retry_status == 200);
    EXPECT(log.retry_usage_input == 7);
    EXPECT(log.n_error == 0);
    EXPECT(log.n_done == 1);
    EXPECT(fake.inits == 2);
    EXPECT(fake.finalizes == 1);
    EXPECT(fake.frees == 2);
}

static void test_midstream_death_budget_exhausted(void)
{
    const char *responses[] = {SSE_OK, SSE_OK};
    setenv("HAX_HTTP_MAX_RETRIES", "1", 1);
    struct fake_stream fake = fake_tracking(-1);
    struct event_log log = {0};
    int result = run_scripted(responses, 2, &fake, &log);
    unsetenv("HAX_HTTP_MAX_RETRIES");

    /* The exhausted attempt still finalizes — the path where real parsers emit
     * the terminal "stream ended before completion" error. */
    EXPECT(result == 0);
    EXPECT(log.n_retry == 1);
    EXPECT(log.retry_usage_input == 0);
    EXPECT(fake.finalizes == 1);
    EXPECT(fake.inits == 2);
    EXPECT(fake.frees == 2);
}

/* A transport error on the final 2xx attempt still delivers the parser's captured usage. */
static void test_transport_error_keeps_captured_usage(void)
{
    const char *responses[] = {SSE_TRUNCATED};
    setenv("HAX_HTTP_MAX_RETRIES", "0", 1);
    struct fake_stream fake = fake_tracking(7);
    struct event_log log = {0};
    int result = run_scripted(responses, 1, &fake, &log);
    unsetenv("HAX_HTTP_MAX_RETRIES");

    EXPECT(result != 0);
    EXPECT(log.n_retry == 0);
    EXPECT(log.n_error == 1);
    EXPECT(log.error_status == 200);
    EXPECT(log.error_usage_input == 7);
    EXPECT(fake.finalizes == 0);
    EXPECT(fake.frees == 1);
}

/* A transport error after the terminal state does not fail the response: it finalizes like
 * a clean close, so its terminal event (and usage) is delivered exactly once. */
static void test_transport_error_after_terminal_finalizes(void)
{
    const char *responses[] = {SSE_TERMINAL_TRUNCATED};
    setenv("HAX_HTTP_MAX_RETRIES", "0", 1);
    struct fake_stream fake = fake_tracking(7);
    struct event_log log = {0};
    int result = run_scripted(responses, 1, &fake, &log);
    unsetenv("HAX_HTTP_MAX_RETRIES");

    EXPECT(result != 0);
    EXPECT(log.n_retry == 0);
    EXPECT(log.n_error == 0);
    EXPECT(log.n_done == 1);
    EXPECT(fake.finalizes == 1);
    EXPECT(fake.frees == 1);
}

static void test_error_message_hook(void)
{
    const char *responses[] = {
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 5\r\nConnection: close\r\n\r\nstale",
    };
    struct fake_stream fake = {.custom_error = "token expired — run /login"};
    struct event_log log = {0};
    int result = run_scripted(responses, 1, &fake, &log);

    EXPECT(result == 0);
    EXPECT(fake.recover_calls == 1);
    EXPECT(log.n_error == 1);
    EXPECT(log.error_status == 401);
    EXPECT_STR_EQ(log.error_message, "token expired — run /login");
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    setenv("HAX_HTTP_RETRY_BASE", "1ms", 1);
    test_success_first_attempt();
    test_retry_then_success();
    test_non_retryable_error();
    test_overflow_refusal_is_classified();
    test_retry_budget_exhausted();
    test_recover_redoes_attempt();
    test_midstream_death_retried();
    test_midstream_death_budget_exhausted();
    test_transport_error_keeps_captured_usage();
    test_transport_error_after_terminal_finalizes();
    test_error_message_hook();
    unsetenv("HAX_HTTP_RETRY_BASE");
    T_REPORT();
}
