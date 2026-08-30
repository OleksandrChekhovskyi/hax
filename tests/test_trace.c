/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "harness.h"
#include "trace.h"
#include "system/fs.h"

/* HAX_TRACE must never write a credential to disk. Two exact rules: the protocol auth headers
 * (Authorization, x-api-key, api-key) are redacted by case-insensitive name, and a value
 * registered via trace_register_secret — every $VAR-resolved header value and API key — is
 * redacted under any header name. Other headers pass through verbatim. */
static void test_credential_headers_redacted(void)
{
    /* Worker-side trace calls are inert until the foreground initializes the
     * destination; they never lazily resolve config themselves. */
    EXPECT(!trace_enabled());

    char path[] = "/tmp/hax_trace_testXXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    if (fd >= 0)
        close(fd);

    /* Point the trace at our temp file and initialize it explicitly. */
    config_set_override("trace", path);
    trace_init();
    EXPECT(trace_enabled());

    trace_register_secret("PORTKEYSECRET");
    trace_register_secret(NULL); /* ignored, not a crash */
    trace_register_secret("");

    const char *headers[] = {
        "x-api-key: sk-ant-SECRETVALUE",           /* Anthropic */
        "Authorization: Bearer BEARERSECRET",      /* OpenAI/Codex */
        "API-Key: AZURESECRET",                    /* Azure, mixed case */
        "x-portkey-api-key: PORTKEYSECRET",        /* registered ($VAR-resolved) value */
        "X-Custom: prefix PORTKEYSECRET embedded", /* registered value inside a larger value */
        "X-Unregistered: LITERALVALUE",            /* literal config value: not recognized */
        "anthropic-version: 2023-06-01",           /* non-secret */
        "Content-Type: application/json",
        NULL,
    };
    trace_request("POST", "https://api.anthropic.com/v1/messages", headers, "{}", 2);

    size_t len = 0;
    char *contents = slurp_file(path, &len);
    EXPECT(contents != NULL);
    if (contents) {
        /* No secret value reaches the file. */
        EXPECT(strstr(contents, "sk-ant-SECRETVALUE") == NULL);
        EXPECT(strstr(contents, "BEARERSECRET") == NULL);
        EXPECT(strstr(contents, "AZURESECRET") == NULL);
        EXPECT(strstr(contents, "PORTKEYSECRET") == NULL);
        /* The header names survive, with redacted values. */
        EXPECT(strstr(contents, "x-api-key: <redacted>") != NULL);
        EXPECT(strstr(contents, "Authorization: <redacted>") != NULL);
        EXPECT(strstr(contents, "API-Key: <redacted>") != NULL);
        EXPECT(strstr(contents, "x-portkey-api-key: <redacted>") != NULL);
        EXPECT(strstr(contents, "X-Custom: <redacted>") != NULL);
        /* Unregistered literals and non-secret headers pass through verbatim. */
        EXPECT(strstr(contents, "X-Unregistered: LITERALVALUE") != NULL);
        EXPECT(strstr(contents, "anthropic-version: 2023-06-01") != NULL);
        free(contents);
    }

    /* Registered values are also replaced inside request and error bodies, where OAuth token
     * requests carry them as JSON values rather than headers. */
    static const char token_body[] =
        "{\"grant_type\":\"refresh_token\",\"refresh_token\":\"PORTKEYSECRET\"}";
    trace_request("POST", "https://auth.example.com/oauth/token", NULL, token_body,
                  sizeof(token_body) - 1);
    trace_response_status(400, "{\"error\":\"bad token PORTKEYSECRET\"}");

    contents = slurp_file(path, &len);
    EXPECT(contents != NULL);
    if (contents) {
        EXPECT(strstr(contents, "PORTKEYSECRET") == NULL);
        EXPECT(strstr(contents, "\"refresh_token\": \"<redacted>\"") != NULL);
        EXPECT(strstr(contents, "bad token <redacted>") != NULL);
        free(contents);
    }

    /* A secret that extends an earlier-registered one must be redacted whole, not left with the
     * unshared tail exposed. */
    trace_register_secret("ROTATED");
    trace_register_secret("ROTATEDLONGER");
    static const char overlap_body[] = "{\"token\":\"ROTATEDLONGER\"}";
    trace_request("POST", "https://auth.example.com/oauth/token", NULL, overlap_body,
                  sizeof(overlap_body) - 1);

    contents = slurp_file(path, &len);
    EXPECT(contents != NULL);
    if (contents) {
        EXPECT(strstr(contents, "LONGER") == NULL);
        EXPECT(strstr(contents, "\"token\": \"<redacted>\"") != NULL);
        free(contents);
    }
    unlink(path);
}

int main(void)
{
    test_credential_headers_redacted();
    T_REPORT();
}
