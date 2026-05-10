/* SPDX-License-Identifier: MIT */
#include "retry.h"

#include <stdlib.h>
#include <time.h>

#include "util.h"

/* Defaults tuned for an interactive CLI: prefer a slightly slower
 * successful reply over a fast clean failure. With 4 retries and
 * 1s base, the cumulative wait across a flaky window is ~15s
 * (1+2+4+8s, before jitter), which rides out a typical provider
 * deploy / rate-limit cooldown / brief 5xx spike. The final per-
 * attempt delay (8s) is the "go get coffee" range without crossing
 * into "is hax frozen?" territory. The max-delay cap at 30s applies
 * when users override HAX_HTTP_RETRY_BASE upward. */
#define DEFAULT_MAX_ATTEMPTS  5 /* one initial + 4 retries */
#define DEFAULT_BASE_DELAY_MS 1000
#define DEFAULT_MAX_DELAY_MS  30000

struct retry_policy retry_policy_default(void)
{
    struct retry_policy p = {
        .max_attempts = DEFAULT_MAX_ATTEMPTS,
        .base_delay_ms = DEFAULT_BASE_DELAY_MS,
        .max_delay_ms = DEFAULT_MAX_DELAY_MS,
    };

    const char *e = getenv("HAX_HTTP_MAX_RETRIES");
    int n;
    if (e && *e && parse_int(e, &n) && n >= 0) {
        /* The env knob is "additional retries"; the policy's max_attempts
         * is total tries including the first, so add one. Cap at a value
         * users would never actually want to wait through. */
        if (n > 100)
            n = 100;
        p.max_attempts = n + 1;
    }

    long base = parse_duration_ms(getenv("HAX_HTTP_RETRY_BASE"));
    if (base > 0)
        p.base_delay_ms = base;

    return p;
}

int retry_should_attempt(int rc, long status)
{
    /* Success — caller doesn't retry, but be defensive. */
    if (rc == 0 && status >= 200 && status < 300)
        return 0;
    /* No status reported = libcurl never got a response line: DNS lookup
     * failed, connect refused/reset, write/recv error during the request
     * phase, etc. All of these are safe to retry. */
    if (status == 0)
        return 1;
    /* 2xx + rc != 0 means we got headers and started streaming, then the
     * connection broke. Some events have already fired through the
     * caller's stream_cb; replaying would duplicate them. The reference
     * implementations handle this case with explicit state replay; we
     * don't, so leave it as a hard failure for now. */
    if (status >= 200 && status < 300)
        return 0;
    /* Transient server-side errors. 408 (request timeout), 429 (rate
     * limit), 5xx (server overload, gateway issues). */
    if (status == 408 || status == 429)
        return 1;
    if (status >= 500 && status <= 599)
        return 1;
    /* 4xx other than the above are permanent for our purposes — auth,
     * bad request, model not found. The caller surfaces them as-is so
     * the user can act on them. */
    return 0;
}

long retry_delay_ms(const struct retry_policy *pol, int attempt)
{
    if (attempt < 0)
        attempt = 0;
    /* Clamp the shift so the doubling doesn't overflow on a configured
     * runaway max_attempts. 30 doublings is already past any sensible
     * max_delay_ms cap. */
    int shift = attempt > 30 ? 30 : attempt;
    long raw = pol->base_delay_ms;
    for (int i = 0; i < shift; i++) {
        if (raw > pol->max_delay_ms)
            break;
        raw <<= 1;
    }
    if (raw < pol->base_delay_ms)
        raw = pol->base_delay_ms;
    if (raw > pol->max_delay_ms)
        raw = pol->max_delay_ms;

    /* Jitter +/-25% from monotonic time. Avoids pulling in <stdlib.h>'s
     * rand() (would need seeding) and stays deterministic-enough for
     * a single-user CLI — we just want adjacent retries to land at
     * slightly different millisecond offsets. Re-clamp after applying
     * jitter so the +25% upper end never pushes us past max_delay_ms. */
    long j = monotonic_ms() % 51; /* 0..50 */
    long out = raw * (75 + j) / 100;
    if (out < 0)
        out = pol->base_delay_ms;
    if (out > pol->max_delay_ms)
        out = pol->max_delay_ms;
    return out;
}

int retry_sleep_with_tick(long ms, http_tick_cb tick, void *user)
{
    if (ms <= 0)
        return tick && tick(user);
    long deadline = monotonic_ms() + ms;
    while (1) {
        if (tick && tick(user))
            return 1;
        long now = monotonic_ms();
        if (now >= deadline)
            return 0;
        long left = deadline - now;
        /* 100 ms slices: short enough that a user Esc shows up promptly
         * via the next tick poll, long enough that the loop overhead
         * stays negligible compared to the actual wait. */
        if (left > 100)
            left = 100;
        struct timespec ts = {
            .tv_sec = left / 1000,
            .tv_nsec = (left % 1000) * 1000000L,
        };
        nanosleep(&ts, NULL);
    }
}
