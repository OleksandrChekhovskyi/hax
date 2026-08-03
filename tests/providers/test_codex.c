/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "harness.h"
#include "providers/codex.h"

static void test_token_expired(void)
{
    char *message = codex_model_catalog_error(401);
    EXPECT_STR_EQ(message, "codex token expired — run `codex` once to refresh, then retry");
    free(message);
}

static void test_empty_success_response(void)
{
    char *message = codex_model_catalog_error(200);
    EXPECT_STR_EQ(message, "codex sent an empty or truncated model catalog response");
    free(message);
}

static void test_http_error(void)
{
    char *message = codex_model_catalog_error(503);
    EXPECT_STR_EQ(message, "codex model catalog fetch failed (HTTP 503)");
    free(message);
}

static void test_unreachable(void)
{
    char *message = codex_model_catalog_error(0);
    EXPECT_STR_EQ(message, "could not reach chatgpt.com to list models — check your network");
    free(message);
}

int main(void)
{
    test_token_expired();
    test_empty_success_response();
    test_http_error();
    test_unreachable();
    T_REPORT();
}
