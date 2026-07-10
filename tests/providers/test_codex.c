/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "harness.h"
#include "providers/codex.h"

/* codex_models_error keys the /model catalog-fetch diagnostic by HTTP
 * status. The 401 wording is a contract shared with the streaming and
 * /usage paths — a stale OAuth token must always point at re-running
 * `codex`, not at a generic connectivity guess. */

static void test_token_expired(void)
{
    char *m = codex_models_error(401);
    EXPECT_STR_EQ(m, "codex token expired — run `codex` once to refresh, then retry");
    free(m);
}

static void test_empty_2xx(void)
{
    /* http_get fails a 2xx with an empty/truncated body — must not
     * render as "failed (HTTP 200)". */
    char *m = codex_models_error(200);
    EXPECT_STR_EQ(m, "codex sent an empty or truncated model catalog response");
    free(m);
}

static void test_http_status(void)
{
    char *m = codex_models_error(503);
    EXPECT_STR_EQ(m, "codex model catalog fetch failed (HTTP 503)");
    free(m);
}

static void test_unreachable(void)
{
    char *m = codex_models_error(0);
    EXPECT_STR_EQ(m, "could not reach chatgpt.com to list models — check your network");
    free(m);
}

int main(void)
{
    test_token_expired();
    test_empty_2xx();
    test_http_status();
    test_unreachable();
    T_REPORT();
}
