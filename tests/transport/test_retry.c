/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "harness.h"
#include "util.h"
#include "transport/retry.h"

static void test_response_classification(void)
{
    EXPECT(retry_should_attempt(0, 200, NULL) == 0);
    EXPECT(retry_should_attempt(0, 204, NULL) == 0);

    EXPECT(retry_should_attempt(-1, 0, NULL) == 1);

    EXPECT(retry_should_attempt(-1, 408, NULL) == 1);
    EXPECT(retry_should_attempt(-1, 429, NULL) == 1);
    EXPECT(retry_should_attempt(-1, 500, NULL) == 1);
    EXPECT(retry_should_attempt(-1, 502, NULL) == 1);
    EXPECT(retry_should_attempt(-1, 503, NULL) == 1);
    EXPECT(retry_should_attempt(-1, 504, NULL) == 1);
    EXPECT(retry_should_attempt(-1, 599, NULL) == 1);

    EXPECT(retry_should_attempt(-1, 400, NULL) == 0);
    EXPECT(retry_should_attempt(-1, 401, NULL) == 0);
    EXPECT(retry_should_attempt(-1, 403, NULL) == 0);
    EXPECT(retry_should_attempt(-1, 404, NULL) == 0);

    EXPECT(retry_should_attempt(-1, 200, NULL) == 0);
    EXPECT(retry_should_attempt(-1, 201, NULL) == 0);
}

static void test_terminal_429_errors(void)
{
    const char *codex_usage =
        "{\"error\":{\"type\":\"usage_limit_reached\",\"plan_type\":\"Pro\",\"resets_at\":1}}";
    EXPECT(retry_should_attempt(-1, 429, codex_usage) == 0);

    const char *not_included = "{\"error\":{\"type\":\"usage_not_included\"}}";
    EXPECT(retry_should_attempt(-1, 429, not_included) == 0);

    const char *quota = "{\"error\":{\"message\":\"You exceeded your current quota\","
                        "\"type\":\"insufficient_quota\",\"code\":\"insufficient_quota\"}}";
    EXPECT(retry_should_attempt(-1, 429, quota) == 0);

    const char *uppercase_quota = "{\"error\":{\"code\":\"INSUFFICIENT_QUOTA\"}}";
    EXPECT(retry_should_attempt(-1, 429, uppercase_quota) == 0);

    const char *opencode_go_usage =
        "{\"type\":\"error\",\"error\":{\"type\":\"GoUsageLimitError\","
        "\"message\":\"5-hour usage limit reached. Resets in 3hr 52min.\"},"
        "\"metadata\":{\"limitName\":\"5 hour\"}}";
    EXPECT(retry_should_attempt(-1, 429, opencode_go_usage) == 0);

    const char *rate_limit =
        "{\"error\":{\"message\":\"Rate limit\",\"type\":\"rate_limit_exceeded\","
        "\"code\":\"rate_limit_exceeded\"}}";
    EXPECT(retry_should_attempt(-1, 429, rate_limit) == 1);

    EXPECT(retry_should_attempt(-1, 429, "{\"error\":{\"message\":\"slow down\"}}") == 1);
    EXPECT(retry_should_attempt(-1, 429, "") == 1);
    EXPECT(retry_should_attempt(-1, 429, "<html>429 Too Many Requests</html>") == 1);

    /* Body only suppresses retry on 429; a 503 with the same marker is
     * still classified by status (transient). Worth pinning explicitly
     * so the override doesn't accidentally bleed into 5xx. */
    EXPECT(retry_should_attempt(-1, 503, codex_usage) == 1);
}

static void test_backoff_growth(void)
{
    struct retry_policy policy = {.max_attempts = 5, .base_delay_ms = 100, .max_delay_ms = 10000};
    long minimum_delay_ms = policy.base_delay_ms * 75 / 100;
    for (int i = 0; i < 8; i++) {
        long delay_ms = retry_delay_ms(&policy, i);
        EXPECT(delay_ms >= minimum_delay_ms);
        EXPECT(delay_ms <= policy.max_delay_ms);
    }
    long first_delay = retry_delay_ms(&policy, 0);
    EXPECT(first_delay >= 75);
    EXPECT(first_delay <= 125);
}

static void test_backoff_cap(void)
{
    struct retry_policy policy = {
        .max_attempts = 100, .base_delay_ms = 1000, .max_delay_ms = 30000};
    long delay_ms = retry_delay_ms(&policy, 50);
    EXPECT(delay_ms <= policy.max_delay_ms);
    EXPECT(delay_ms > 0);
}

static void test_default_policy_config(void)
{
    unsetenv("HAX_HTTP_MAX_RETRIES");
    unsetenv("HAX_HTTP_RETRY_BASE");
    unsetenv("HAX_HTTP_IDLE_TIMEOUT");
    struct retry_policy defaults = retry_policy_default();
    EXPECT(defaults.max_attempts == 5);
    EXPECT(defaults.base_delay_ms == 1000);
    EXPECT(defaults.idle_timeout_s == 10 * 60);

    setenv("HAX_HTTP_MAX_RETRIES", "7", 1);
    struct retry_policy override = retry_policy_default();
    EXPECT(override.max_attempts == 8);

    setenv("HAX_HTTP_MAX_RETRIES", "0", 1);
    struct retry_policy no_retries = retry_policy_default();
    EXPECT(no_retries.max_attempts == 1);

    /* Override base delay via parse_duration_ms grammar — "ms" suffix
     * because bare numbers parse as seconds. */
    setenv("HAX_HTTP_RETRY_BASE", "200ms", 1);
    struct retry_policy millisecond_base = retry_policy_default();
    EXPECT(millisecond_base.base_delay_ms == 200);

    setenv("HAX_HTTP_RETRY_BASE", "2", 1);
    struct retry_policy second_base = retry_policy_default();
    EXPECT(second_base.base_delay_ms == 2000);

    /* Semantically invalid values fall back to the defaults: a negative
     * retry count and a zero base delay are typos, not meanings. */
    setenv("HAX_HTTP_MAX_RETRIES", "-1", 1);
    struct retry_policy negative_retries = retry_policy_default();
    EXPECT(negative_retries.max_attempts == 5);

    setenv("HAX_HTTP_RETRY_BASE", "0", 1);
    struct retry_policy zero_base = retry_policy_default();
    EXPECT(zero_base.base_delay_ms == 1000);

    /* The transport receives whole seconds: non-zero sub-second values round
     * up, while zero retains its explicit "disabled" meaning. */
    setenv("HAX_HTTP_IDLE_TIMEOUT", "500ms", 1);
    struct retry_policy subsecond = retry_policy_default();
    EXPECT(subsecond.idle_timeout_s == 1);
    setenv("HAX_HTTP_IDLE_TIMEOUT", "0", 1);
    struct retry_policy disabled = retry_policy_default();
    EXPECT(disabled.idle_timeout_s == 0);

    unsetenv("HAX_HTTP_MAX_RETRIES");
    unsetenv("HAX_HTTP_RETRY_BASE");
    unsetenv("HAX_HTTP_IDLE_TIMEOUT");
}

static int always_cancel(void *user)
{
    int *calls = user;
    (*calls)++;
    return 1;
}

static int never_cancel(void *user)
{
    int *calls = user;
    (*calls)++;
    return 0;
}

static void test_sleep_cancellation(void)
{
    int calls = 0;
    long started_ms = monotonic_ms();
    int result = retry_sleep_with_tick(5000, always_cancel, &calls);
    long elapsed_ms = monotonic_ms() - started_ms;
    EXPECT(result == 1);
    EXPECT(calls >= 1);
    EXPECT(elapsed_ms < 200);
}

static void test_sleep_completes(void)
{
    int calls = 0;
    long started_ms = monotonic_ms();
    int result = retry_sleep_with_tick(150, never_cancel, &calls);
    long elapsed_ms = monotonic_ms() - started_ms;
    EXPECT(result == 0);
    EXPECT(elapsed_ms >= 100);
    EXPECT(elapsed_ms < 1500);
    EXPECT(calls >= 1);
}

int main(void)
{
    test_response_classification();
    test_terminal_429_errors();
    test_backoff_growth();
    test_backoff_cap();
    test_default_policy_config();
    test_sleep_cancellation();
    test_sleep_completes();
    T_REPORT();
}
