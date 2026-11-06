/* SPDX-License-Identifier: MIT */
#include <stddef.h>

#include "harness.h"
#include "provider_router.h"
#include "providers/registry.h"
#include "transport/retry.h"

/* Minimal provider definition for testing router construction. */
static struct provider_def make_def(const char *id, int pinned)
{
    return (struct provider_def){
        .id = id,
        .pinned = pinned,
        .api = "openai-completions",
        .api_key_env = NULL,
    };
}

static void test_is_terminal_error_classification(void)
{
    /* Mirrors the classification in retry_is_terminal_error used by the router. */
    EXPECT(retry_is_terminal_error(0, NULL) == 0);   /* transport -> retry, not terminal */
    EXPECT(retry_is_terminal_error(200, NULL) == 0); /* success */
    EXPECT(retry_is_terminal_error(401, NULL) == 1); /* auth failure -> terminal */
    EXPECT(retry_is_terminal_error(403, NULL) == 1); /* forbidden -> terminal */
    EXPECT(retry_is_terminal_error(408, NULL) == 0); /* timeout -> retry */
    EXPECT(retry_is_terminal_error(429, NULL) == 0); /* rate limit without quota -> retry */
    EXPECT(retry_is_terminal_error(500, NULL) == 0); /* server error -> retry */
    EXPECT(retry_is_terminal_error(503, NULL) == 0); /* service unavailable -> retry */
}

static void test_router_position_non_router(void)
{
    (void)make_def("fake", 1);
    /* Router position is only meaningful for router providers. */
    EXPECT(provider_router_position(NULL, NULL, NULL) == 0);
}

int main(void)
{
    test_is_terminal_error_classification();
    test_router_position_non_router();
    T_REPORT();
}