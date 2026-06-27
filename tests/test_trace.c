/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "harness.h"
#include "trace.h"
#include "util.h"

/* HAX_TRACE must never write a credential to disk. The dump redacts a fixed
 * set of auth header names (Authorization, x-api-key, api-key) case-
 * insensitively, while passing non-secret headers through verbatim. */
static void test_credential_headers_redacted(void)
{
    char path[] = "/tmp/hax_trace_testXXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    if (fd >= 0)
        close(fd);

    /* Point the trace at our temp file and force the lazy open. */
    config_set_override("trace", path);
    trace_init();
    EXPECT(trace_enabled());

    const char *headers[] = {
        "x-api-key: sk-ant-SECRETVALUE",      /* Anthropic */
        "Authorization: Bearer BEARERSECRET", /* OpenAI/Codex */
        "API-Key: AZURESECRET",               /* Azure, mixed case */
        "anthropic-version: 2023-06-01",      /* non-secret */
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
        /* The header names survive, with redacted values. */
        EXPECT(strstr(contents, "x-api-key: <redacted>") != NULL);
        EXPECT(strstr(contents, "Authorization: <redacted>") != NULL);
        EXPECT(strstr(contents, "API-Key: <redacted>") != NULL);
        /* Non-secret headers pass through verbatim. */
        EXPECT(strstr(contents, "anthropic-version: 2023-06-01") != NULL);
        free(contents);
    }
    unlink(path);
}

int main(void)
{
    test_credential_headers_redacted();
    T_REPORT();
}
