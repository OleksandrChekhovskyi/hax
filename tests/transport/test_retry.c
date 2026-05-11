/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "harness.h"
#include "util.h"
#include "transport/retry.h"

static void test_classify(void)
{
    /* Success — never retry. */
    EXPECT(retry_should_attempt(0, 200, NULL) == 0);
    EXPECT(retry_should_attempt(0, 204, NULL) == 0);

    /* Transport errors (no status from libcurl) — retry. */
    EXPECT(retry_should_attempt(-1, 0, NULL) == 1);

    /* Transient HTTP — retry. */
    EXPECT(retry_should_attempt(-1, 408, NULL) == 1);
    EXPECT(retry_should_attempt(-1, 429, NULL) == 1);
    EXPECT(retry_should_attempt(-1, 500, NULL) == 1);
    EXPECT(retry_should_attempt(-1, 502, NULL) == 1);
    EXPECT(retry_should_attempt(-1, 503, NULL) == 1);
    EXPECT(retry_should_attempt(-1, 504, NULL) == 1);
    EXPECT(retry_should_attempt(-1, 599, NULL) == 1);

    /* Permanent client errors — don't retry. */
    EXPECT(retry_should_attempt(-1, 400, NULL) == 0);
    EXPECT(retry_should_attempt(-1, 401, NULL) == 0);
    EXPECT(retry_should_attempt(-1, 403, NULL) == 0);
    EXPECT(retry_should_attempt(-1, 404, NULL) == 0);

    /* Mid-stream drop (2xx + non-zero rc) — don't retry, would dup output. */
    EXPECT(retry_should_attempt(-1, 200, NULL) == 0);
    EXPECT(retry_should_attempt(-1, 201, NULL) == 0);
}

static void test_429_body_terminal(void)
{
    /* Codex subscription cap — error.type marks it terminal. */
    const char *codex_usage =
        "{\"error\":{\"type\":\"usage_limit_reached\",\"plan_type\":\"Pro\",\"resets_at\":1}}";
    EXPECT(retry_should_attempt(-1, 429, codex_usage) == 0);

    /* Codex plan doesn't include Codex — also terminal. */
    const char *not_included = "{\"error\":{\"type\":\"usage_not_included\"}}";
    EXPECT(retry_should_attempt(-1, 429, not_included) == 0);

    /* OpenAI hard quota — error.code (not type). */
    const char *quota = "{\"error\":{\"message\":\"You exceeded your current quota\","
                        "\"type\":\"insufficient_quota\",\"code\":\"insufficient_quota\"}}";
    EXPECT(retry_should_attempt(-1, 429, quota) == 0);

    /* Case-insensitive match. */
    const char *upper = "{\"error\":{\"code\":\"INSUFFICIENT_QUOTA\"}}";
    EXPECT(retry_should_attempt(-1, 429, upper) == 0);

    /* Per-minute rate limit — transient, still retryable. */
    const char *rl = "{\"error\":{\"message\":\"Rate limit\",\"type\":\"rate_limit_exceeded\","
                     "\"code\":\"rate_limit_exceeded\"}}";
    EXPECT(retry_should_attempt(-1, 429, rl) == 1);

    /* Unknown / generic 429 body — fall back to retry. */
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
    struct retry_policy pol = {.max_attempts = 5, .base_delay_ms = 100, .max_delay_ms = 10000};
    /* Each attempt should be >= the base, monotonically non-decreasing
     * within jitter bounds, and capped at max_delay_ms. With +/-25%
     * jitter, attempt N's worst case (75%) is still > attempt N-1's
     * worst case once the doubling has had room to act, so just check
     * they're all in the [base*0.75, max] window. */
    long min = pol.base_delay_ms * 75 / 100;
    for (int i = 0; i < 8; i++) {
        long d = retry_delay_ms(&pol, i);
        EXPECT(d >= min);
        EXPECT(d <= pol.max_delay_ms);
    }
    /* At i=0 the raw value is base, so the result is base * (0.75..1.25). */
    long d0 = retry_delay_ms(&pol, 0);
    EXPECT(d0 >= 75);
    EXPECT(d0 <= 125);
}

static void test_backoff_cap(void)
{
    /* Even with absurdly many attempts, the delay never exceeds the cap. */
    struct retry_policy pol = {.max_attempts = 100, .base_delay_ms = 1000, .max_delay_ms = 30000};
    long d = retry_delay_ms(&pol, 50);
    EXPECT(d <= pol.max_delay_ms);
    EXPECT(d > 0);
}

static void test_default_env(void)
{
    /* Unset → defaults applied. */
    unsetenv("HAX_HTTP_MAX_RETRIES");
    unsetenv("HAX_HTTP_RETRY_BASE");
    struct retry_policy d = retry_policy_default();
    EXPECT(d.max_attempts == 5);
    EXPECT(d.base_delay_ms == 1000);

    /* Override max_retries (count of retries, not total attempts). */
    setenv("HAX_HTTP_MAX_RETRIES", "7", 1);
    struct retry_policy o = retry_policy_default();
    EXPECT(o.max_attempts == 8);

    /* 0 retries means a single attempt. */
    setenv("HAX_HTTP_MAX_RETRIES", "0", 1);
    struct retry_policy z = retry_policy_default();
    EXPECT(z.max_attempts == 1);

    /* Override base delay via parse_duration_ms grammar — "ms" suffix
     * because bare numbers parse as seconds. */
    setenv("HAX_HTTP_RETRY_BASE", "200ms", 1);
    struct retry_policy b = retry_policy_default();
    EXPECT(b.base_delay_ms == 200);

    /* Bare number = seconds (matches HAX_HTTP_IDLE_TIMEOUT). */
    setenv("HAX_HTTP_RETRY_BASE", "2", 1);
    struct retry_policy s = retry_policy_default();
    EXPECT(s.base_delay_ms == 2000);

    unsetenv("HAX_HTTP_MAX_RETRIES");
    unsetenv("HAX_HTTP_RETRY_BASE");
}

static int always_cancel(void *user)
{
    int *n = user;
    (*n)++;
    return 1;
}

static int never_cancel(void *user)
{
    int *n = user;
    (*n)++;
    return 0;
}

static void test_sleep_cancellation(void)
{
    /* Cancel-immediately tick aborts before any real sleep elapses. */
    int calls = 0;
    long t0 = monotonic_ms();
    int rc = retry_sleep_with_tick(5000, always_cancel, &calls);
    long elapsed = monotonic_ms() - t0;
    EXPECT(rc == 1);
    EXPECT(calls >= 1);
    EXPECT(elapsed < 200); /* should be near-instant, not ~5s */
}

static void test_sleep_completes(void)
{
    int calls = 0;
    long t0 = monotonic_ms();
    int rc = retry_sleep_with_tick(150, never_cancel, &calls);
    long elapsed = monotonic_ms() - t0;
    EXPECT(rc == 0);
    /* Slept roughly the requested duration. Allow generous slop for CI. */
    EXPECT(elapsed >= 100);
    EXPECT(elapsed < 1500);
    EXPECT(calls >= 1);
}

int main(void)
{
    test_classify();
    test_429_body_terminal();
    test_backoff_growth();
    test_backoff_cap();
    test_default_env();
    test_sleep_cancellation();
    test_sleep_completes();
    T_REPORT();
}
