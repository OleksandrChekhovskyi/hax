/* SPDX-License-Identifier: MIT */
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <sys/stat.h>

#include "config.h"
#include "harness.h"
#include "loopback.h"
#include "trace.h"
#include "xalloc.h"
#include "providers/http_provider.h"
#include "providers/vertex_auth.h"
#include "system/clock.h"
#include "system/fs.h"

static void write_adc(const char *contents)
{
    char *path = xasprintf("%s/adc.json", t_tempdir());
    FILE *file = fopen(path, "w");
    EXPECT(file != NULL);
    if (file) {
        fputs(contents, file);
        fclose(file);
    }
    setenv("GOOGLE_APPLICATION_CREDENTIALS", path, 1);
    free(path);
}

static char *gcloud_saved_path;

static void install_gcloud_stub(const char *body)
{
    char *dir = t_tempdir();
    char *stub = xasprintf("%s/gcloud", dir);
    char *script = xasprintf("#!/bin/sh\n%s\n", body);
    EXPECT(fs_write_atomic(stub, script, strlen(script), 0) == 0);
    free(script);
    EXPECT(chmod(stub, 0700) == 0);
    free(stub);
    gcloud_saved_path = t_path_prepend(dir);
}

static void restore_gcloud_stub(void)
{
    if (gcloud_saved_path) {
        t_path_restore(gcloud_saved_path);
        gcloud_saved_path = NULL;
    }
}

static const char USER_ADC[] = "{\"type\":\"authorized_user\",\"client_id\":\"cid\","
                               "\"client_secret\":\"secret\",\"refresh_token\":\"refresh\"}";

static void expect_token_redacted(const struct http_auth_source *source, const char *token,
                                  const char *trace_path)
{
    char **headers = source->ops->headers(source->state, "sid", 1);
    char *authorization = xasprintf("Authorization: Bearer %s", token);
    EXPECT_STR_EQ(headers[0], authorization);
    free(authorization);
    string_array_free(headers);

    char *error = xasprintf("rejected access token %s", token);
    trace_response_status(401, error);
    free(error);
    char *contents = fs_read_file(trace_path, NULL);
    EXPECT(contents != NULL);
    if (contents) {
        EXPECT(strstr(contents, token) == NULL);
        EXPECT(strstr(contents, "rejected access token <redacted>") != NULL);
        free(contents);
    }
}

static void test_literal_trace_redaction(const char *trace_path)
{
    setenv("GOOGLE_OAUTH_ACCESS_TOKEN", "google-access-token", 1);
    config_set_override("providers.vertex.access_token", "configured-access-token");
    struct http_auth_source source = {0};
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == 0);
    expect_token_redacted(&source, "configured-access-token", trace_path);
    source.ops->destroy(source.state);
    config_set_override("providers.vertex.access_token", NULL);

    setenv("HAX_VERTEX_ACCESS_TOKEN", "hax-access-token", 1);
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == 0);
    expect_token_redacted(&source, "hax-access-token", trace_path);
    source.ops->destroy(source.state);

    setenv("HAX_VERTEX_ACCESS_TOKEN", "", 1);
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == 0);
    expect_token_redacted(&source, "google-access-token", trace_path);
    source.ops->destroy(source.state);
    unsetenv("HAX_VERTEX_ACCESS_TOKEN");
    unsetenv("GOOGLE_OAUTH_ACCESS_TOKEN");
}

static void test_oauth_trace_redaction(const char *trace_path)
{
    static const char error[] =
        "raw-secret=client+secret&100%; encoded-secret=client%2Bsecret%26100%25; "
        "raw-refresh=refresh+token&50%; encoded-refresh=refresh%2Btoken%2650%25";
    char *error_response = xasprintf("HTTP/1.1 400 Bad Request\r\nContent-Length: %zu\r\n"
                                     "Connection: close\r\n\r\n%s",
                                     strlen(error), error);
    struct loopback server = {.n_requests = 2};
    loopback_reply_ok(&server, 0, "{\"access_token\":\"oauth-access-token\",\"expires_in\":3600}");
    server.responses[1] = error_response;
    int port = loopback_start(&server);
    EXPECT(port > 0);
    if (port <= 0)
        goto out_server;

    char *url = xasprintf("http://127.0.0.1:%d/token", port);
    setenv("HAX_VERTEX_OAUTH_URL", url, 1);
    free(url);
    write_adc("{\"type\":\"authorized_user\",\"client_id\":\"client+id&25%\","
              "\"client_secret\":\"client+secret&100%\",\"refresh_token\":\"refresh+token&50%\"}");

    struct http_auth_source source = {0};
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == 0);
    expect_token_redacted(&source, "oauth-access-token", trace_path);
    int retried = 0;
    EXPECT(source.ops->recover(source.state, &retried, NULL, NULL) == 0);
    EXPECT(retried == 1);
    source.ops->destroy(source.state);
    unsetenv("HAX_VERTEX_OAUTH_URL");

    loopback_stop(&server);
    EXPECT(atomic_load(&server.served) == 2);
    for (int i = 0; i < 2; i++) {
        EXPECT(strstr(server.requests[i], "POST /token HTTP/1.1") != NULL);
        EXPECT(strstr(server.requests[i], "Content-Type: application/x-www-form-urlencoded") !=
               NULL);
        EXPECT(strstr(server.requests[i], "grant_type=refresh_token&client_id=client%2Bid%2625%25&"
                                          "client_secret=client%2Bsecret%26100%25&"
                                          "refresh_token=refresh%2Btoken%2650%25") != NULL);
    }

    char *contents = fs_read_file(trace_path, NULL);
    EXPECT(contents != NULL);
    if (contents) {
        EXPECT(strstr(contents, "client+secret&100%") == NULL);
        EXPECT(strstr(contents, "client%2Bsecret%26100%25") == NULL);
        EXPECT(strstr(contents, "refresh+token&50%") == NULL);
        EXPECT(strstr(contents, "refresh%2Btoken%2650%25") == NULL);
        EXPECT(strstr(contents, "client_secret=<redacted>&refresh_token=<redacted>") != NULL);
        EXPECT(strstr(contents, "raw-secret=<redacted>; encoded-secret=<redacted>") != NULL);
        EXPECT(strstr(contents, "raw-refresh=<redacted>; encoded-refresh=<redacted>") != NULL);
        free(contents);
    }

out_server:
    loopback_stop(&server);
    free(error_response);
}

static void test_gcloud_trace_redaction(const char *trace_path)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/gcloud", dir);
    FILE *file = fopen(path, "w");
    EXPECT(file != NULL);
    if (!file) {
        free(path);
        return;
    }
    fputs("#!/bin/sh\n"
          "[ \"$*\" = 'auth application-default print-access-token' ] || exit 1\n"
          "printf 'gcloud-access-token\\n'\n",
          file);
    fclose(file);
    EXPECT(chmod(path, 0700) == 0);
    free(path);
    char *saved_path = t_path_prepend(dir);
    write_adc("{\"type\":\"service_account\"}");

    struct http_auth_source source = {0};
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == 0);
    expect_token_redacted(&source, "gcloud-access-token", trace_path);
    source.ops->destroy(source.state);
    t_path_restore(saved_path);
}

static void test_prepare_no_refresh(void)
{
    struct loopback oauth = {.n_requests = 1};
    loopback_reply_ok(&oauth, 0, "{\"access_token\":\"no-refresh-token\",\"expires_in\":7200}");
    oauth.response = oauth.responses[0];
    int port = loopback_listen(&oauth);
    EXPECT(port > 0);
    if (port <= 0)
        return;
    char *url = xasprintf("http://127.0.0.1:%d/token", port);
    setenv("HAX_VERTEX_OAUTH_URL", url, 1);
    free(url);
    write_adc(USER_ADC);

    struct http_auth_source source = {0};
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    /* Without refresh and without a token, a renewable source fails locally. */
    EXPECT(source.ops->prepare(source.state, 0, NULL, NULL) == -1);
    struct pollfd listener = {.fd = oauth.listener_fd, .events = POLLIN};
    EXPECT(poll(&listener, 1, 0) == 0);

    int rc = loopback_serve(&oauth);
    EXPECT(rc == 0);
    if (rc != 0)
        goto out;
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == 0);
    /* The fresh token is reusable without further network access. */
    EXPECT(source.ops->prepare(source.state, 0, NULL, NULL) == 0);
    loopback_stop(&oauth);
    EXPECT(atomic_load(&oauth.served) == 1);

out:
    source.ops->destroy(source.state);
    unsetenv("HAX_VERTEX_OAUTH_URL");
}

/* A token the margin marks as already expiring must fail a refresh-disabled prepare instead of
 * returning success. */
static void test_prepare_expired_refuses_network(void)
{
    struct loopback oauth = {.n_requests = 1};
    loopback_reply_ok(&oauth, 0, "{\"access_token\":\"short-lived\",\"expires_in\":1}");
    oauth.response = oauth.responses[0];
    int port = loopback_listen(&oauth);
    EXPECT(port > 0);
    if (port <= 0)
        return;
    char *url = xasprintf("http://127.0.0.1:%d/token", port);
    setenv("HAX_VERTEX_OAUTH_URL", url, 1);
    free(url);
    write_adc(USER_ADC);

    struct http_auth_source source = {0};
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    int rc = loopback_serve(&oauth);
    EXPECT(rc == 0);
    if (rc != 0)
        goto out;
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == 0);
    /* The early-refresh margin applies to user tokens: a 1s token is already expiring. */
    EXPECT(source.ops->prepare(source.state, 0, NULL, NULL) == -1);
    EXPECT(source.ops->prepare(source.state, 0, NULL, NULL) == -1);
    loopback_stop(&oauth);
    EXPECT(atomic_load(&oauth.served) == 1); /* only the initial refresh connected */

out:
    source.ops->destroy(source.state);
    unsetenv("HAX_VERTEX_OAUTH_URL");
}

static void test_recover_literal_semantics(void)
{
    setenv("GOOGLE_OAUTH_ACCESS_TOKEN", "literal-one", 1);
    struct http_auth_source source = {0};
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == 0);
    char **headers = source.ops->headers(source.state, "sid", 1);
    EXPECT(strstr(headers[0], "Bearer literal-one") != NULL);
    string_array_free(headers);

    /* An unchanged rejected literal is never resent. */
    int retried = 0;
    EXPECT(source.ops->recover(source.state, &retried, NULL, NULL) == 0);
    EXPECT(retried == 1);

    /* A different literal permits exactly one retry. */
    setenv("GOOGLE_OAUTH_ACCESS_TOKEN", "literal-two", 1);
    retried = 0;
    EXPECT(source.ops->recover(source.state, &retried, NULL, NULL) == 1);
    EXPECT(retried == 1);
    headers = source.ops->headers(source.state, "sid", 1);
    EXPECT(strstr(headers[0], "Bearer literal-two") != NULL);
    string_array_free(headers);
    retried = 0;
    EXPECT(source.ops->recover(source.state, &retried, NULL, NULL) == 0);
    EXPECT(retried == 1);

    source.ops->destroy(source.state);
    unsetenv("GOOGLE_OAUTH_ACCESS_TOKEN");
}

/* Recovery re-reads the source and re-classifies it; a switch to a literal never runs the old
 * gcloud exchange with missing ADC fields. */
static void test_recover_source_transitions(void)
{
    install_gcloud_stub("printf 'service-token\\n'");
    write_adc("{\"type\":\"service_account\"}");
    struct http_auth_source source = {0};
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == 0);
    char **headers = source.ops->headers(source.state, "sid", 1);
    EXPECT(strstr(headers[0], "Bearer service-token") != NULL);
    string_array_free(headers);

    /* The source switches to a literal: the forced renewal adopts it without gcloud. */
    setenv("GOOGLE_OAUTH_ACCESS_TOKEN", "literal-new", 1);
    int retried = 0;
    EXPECT(source.ops->recover(source.state, &retried, NULL, NULL) == 1);
    EXPECT(retried == 1);
    headers = source.ops->headers(source.state, "sid", 1);
    EXPECT(strstr(headers[0], "Bearer literal-new") != NULL);
    string_array_free(headers);

    source.ops->destroy(source.state);
    unsetenv("GOOGLE_OAUTH_ACCESS_TOKEN");
    restore_gcloud_stub();
}

static void test_gcloud_blank_output_rejected(void)
{
    install_gcloud_stub("printf '\\n   \\n'");
    write_adc("{\"type\":\"service_account\"}");
    struct http_auth_source source = {0};
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == -1);
    char *message = source.ops->unauthorized_message(source.state);
    EXPECT(strstr(message, "no usable token") != NULL);
    free(message);
    source.ops->destroy(source.state);
    restore_gcloud_stub();
}

static int cancel_now(void *user)
{
    (void)user;
    return 1;
}

/* A cancelled gcloud capture must abort promptly and reap the child instead of waiting out the
 * bounded timeout. */
static void test_gcloud_refresh_cancellable(void)
{
    install_gcloud_stub("sleep 30");
    write_adc("{\"type\":\"service_account\"}");
    struct http_auth_source source = {0};
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    long started = monotonic_ms();
    EXPECT(source.ops->prepare(source.state, 1, cancel_now, NULL) == -1);
    long elapsed_ms = monotonic_ms() - started;
    EXPECT(elapsed_ms < 5000);
    source.ops->destroy(source.state);
    restore_gcloud_stub();
}

static void test_oauth_invalid_grant(void)
{
    static const char body[] = "{\"error\":\"invalid_grant\"}";
    char *error_response = xasprintf("HTTP/1.1 401 Unauthorized\r\nContent-Length: %zu\r\n"
                                     "Connection: close\r\n\r\n%s",
                                     strlen(body), body);
    struct loopback oauth = {.n_requests = 1, .responses = {error_response}};
    int port = loopback_start(&oauth);
    EXPECT(port > 0);
    if (port <= 0)
        goto out_server;
    char *url = xasprintf("http://127.0.0.1:%d/token", port);
    setenv("HAX_VERTEX_OAUTH_URL", url, 1);
    free(url);
    write_adc(USER_ADC);

    struct http_auth_source source = {0};
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == -1);
    char *message = source.ops->unauthorized_message(source.state);
    EXPECT(strstr(message, "invalid_grant") != NULL);
    EXPECT(strstr(message, "auth application-default login") != NULL);
    free(message);
    source.ops->destroy(source.state);
    unsetenv("HAX_VERTEX_OAUTH_URL");
    loopback_stop(&oauth);

out_server:
    free(error_response);
}

static void test_credential_faults_distinguished(void)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/custom.json", dir);
    struct config_snapshot *saved = config_snapshot_take();

    setenv("GOOGLE_APPLICATION_CREDENTIALS", path, 1);
    struct http_auth_source source = {0};
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == -1);
    char *message = source.ops->unauthorized_message(source.state);
    EXPECT(strstr(message, "no Google ADC credentials") != NULL);
    free(message);
    source.ops->destroy(source.state);

    EXPECT(fs_write_atomic(path, USER_ADC, strlen(USER_ADC), 0) == 0);
    EXPECT(chmod(path, 0) == 0);
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == -1);
    message = source.ops->unauthorized_message(source.state);
    EXPECT(strstr(message, "unreadable") != NULL);
    free(message);
    source.ops->destroy(source.state);
    EXPECT(chmod(path, 0600) == 0);

    EXPECT(fs_write_atomic(path, "{not json", strlen("{not json"), 0) == 0);
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == -1);
    message = source.ops->unauthorized_message(source.state);
    EXPECT(strstr(message, "not valid JSON") != NULL);
    free(message);
    source.ops->destroy(source.state);

    config_snapshot_restore(saved);
    free(path);
}

static void test_setup_diagnostics(void)
{
    char *dir = t_tempdir();
    char *saved_path = t_path_replace(dir);
    char *missing_adc = xasprintf("%s/missing-adc.json", dir);
    setenv("GOOGLE_APPLICATION_CREDENTIALS", missing_adc, 1);
    free(missing_adc);

    struct http_auth_source source = {0};
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == -1);
    char *message = source.ops->unauthorized_message(source.state);
    EXPECT(strstr(message, "gcloud auth application-default login") != NULL);
    EXPECT(strstr(message, "GOOGLE_OAUTH_ACCESS_TOKEN") != NULL);
    free(message);
    source.ops->destroy(source.state);

    write_adc("{\"type\":\"service_account\"}");
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == -1);
    message = source.ops->unauthorized_message(source.state);
    EXPECT(strstr(message, "gcloud not found") != NULL);
    EXPECT(strstr(message, "install the Google Cloud CLI") != NULL);
    EXPECT(strstr(message, "PATH") != NULL);
    EXPECT(strstr(message, "GOOGLE_OAUTH_ACCESS_TOKEN") != NULL);
    free(message);
    source.ops->destroy(source.state);
    t_path_restore(saved_path);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    EXPECT(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    setenv("HOME", t_tempdir(), 1);
    setenv("CLOUDSDK_CONFIG", t_tempdir(), 1);
    setenv("GOOGLE_APPLICATION_CREDENTIALS", "/nonexistent/hax-test-adc.json", 1);
    unsetenv("HAX_VERTEX_ACCESS_TOKEN");
    unsetenv("GOOGLE_OAUTH_ACCESS_TOKEN");
    unsetenv("HAX_VERTEX_OAUTH_URL");
    setenv("NO_PROXY", "*", 1);
    setenv("no_proxy", "*", 1);

    char *trace_path = xasprintf("%s/trace.log", t_tempdir());
    config_set_override("trace", trace_path);
    trace_init();
    EXPECT(trace_enabled());
    test_literal_trace_redaction(trace_path);
    test_oauth_trace_redaction(trace_path);
    test_gcloud_trace_redaction(trace_path);
    test_setup_diagnostics();
    test_credential_faults_distinguished();
    test_prepare_no_refresh();
    test_prepare_expired_refuses_network();
    test_recover_literal_semantics();
    test_recover_source_transitions();
    test_gcloud_blank_output_rejected();
    test_gcloud_refresh_cancellable();
    test_oauth_invalid_grant();
    free(trace_path);
    config_free();
    curl_global_cleanup();
    T_REPORT();
}
