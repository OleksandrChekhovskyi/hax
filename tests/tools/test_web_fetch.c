/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "tool.h"

static char *call(const char *args_json)
{
    return TOOL_WEB_FETCH.run(args_json, NULL, NULL);
}

static void test_invalid_json(void)
{
    char *out = call("not json");
    EXPECT(strstr(out, "invalid arguments") != NULL);
    free(out);
}

static void test_missing_url(void)
{
    char *out = call("{}");
    EXPECT(strstr(out, "missing 'url'") != NULL);
    free(out);
}

static void test_rejects_non_http_scheme(void)
{
    char *out = call("{\"url\":\"file:///etc/passwd\"}");
    EXPECT(strstr(out, "http://") != NULL);
    free(out);

    out = call("{\"url\":\"ftp://example.com/x\"}");
    EXPECT(strstr(out, "http://") != NULL);
    free(out);
}

static void test_rejects_embedded_credentials(void)
{
    char *out = call("{\"url\":\"https://user:pass@example.com/\"}");
    EXPECT(strstr(out, "credentials") != NULL);
    free(out);
}

static void test_at_in_path_is_allowed(void)
{
    /* An '@' after the authority (in the path/query) is not userinfo and must
     * not be rejected — we can't fetch in a unit test, so just confirm it
     * doesn't trip the credential guard. */
    char *out = call("{\"url\":\"https://example.com/u/@handle\"}");
    EXPECT(strstr(out, "credentials") == NULL);
    free(out);
}

static void test_display_extra_raw(void)
{
    char *out = TOOL_WEB_FETCH.format_display_extra("{\"url\":\"https://x.com\",\"raw\":true}");
    EXPECT(out != NULL && strstr(out, "raw") != NULL);
    free(out);

    out = TOOL_WEB_FETCH.format_display_extra("{\"url\":\"https://x.com\"}");
    EXPECT(out == NULL);
    free(out);
}

int main(void)
{
    test_invalid_json();
    test_missing_url();
    test_rejects_non_http_scheme();
    test_rejects_embedded_credentials();
    test_at_in_path_is_allowed();
    test_display_extra_raw();
    T_REPORT();
}
