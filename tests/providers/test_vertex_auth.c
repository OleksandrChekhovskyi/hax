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
#include "providers/vertex.h"
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
    config_set_override("providers.vertex.access_token", "configured-access-token");
    struct http_auth_source source = {0};
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == 0);
    expect_token_redacted(&source, "configured-access-token", trace_path);
    source.ops->destroy(source.state);
    config_set_override("providers.vertex.access_token", NULL);

    setenv("GOOGLE_OAUTH_ACCESS_TOKEN", "environment-access-token", 1);
    EXPECT(vertex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == 0);
    expect_token_redacted(&source, "environment-access-token", trace_path);
    source.ops->destroy(source.state);
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

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    EXPECT(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    setenv("HOME", t_tempdir(), 1);
    setenv("CLOUDSDK_CONFIG", t_tempdir(), 1);
    setenv("GOOGLE_APPLICATION_CREDENTIALS", "/nonexistent/hax-test-adc.json", 1);
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
    free(trace_path);
    config_free();
    curl_global_cleanup();
    T_REPORT();
}
