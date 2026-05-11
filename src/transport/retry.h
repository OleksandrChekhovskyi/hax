/* SPDX-License-Identifier: MIT */
#ifndef HAX_RETRY_H
#define HAX_RETRY_H

#include "transport/http.h"

/* Auto-retry policy for streaming HTTP calls.
 *
 * Scope: pre-stream failures only — transport errors (DNS, connect, reset)
 * and HTTP-level transient errors (408, 429, 5xx). A 2xx response with a
 * later rc != 0 is treated as a mid-stream drop and is NOT retried, since
 * partial events have already been delivered to the caller's callback and
 * a replay would duplicate them. Reference impls (codex's stream_max_retries)
 * handle that case by replaying state; that complexity isn't worth it here.
 *
 * Tunable via HAX_HTTP_MAX_RETRIES (count of additional retries; total
 * attempts is one more) and HAX_HTTP_RETRY_BASE (delay before the
 * second attempt — uses parse_duration_ms grammar, so "500ms", "1s",
 * "2m" all work; bare numbers are seconds, matching
 * HAX_HTTP_IDLE_TIMEOUT). */

struct retry_policy {
    int max_attempts;   /* total tries including the first; 1 = no retry */
    long base_delay_ms; /* delay before the second attempt */
    long max_delay_ms;  /* upper bound after exponential growth + jitter */
};

/* Build the default policy from env (HAX_HTTP_MAX_RETRIES,
 * HAX_HTTP_RETRY_BASE) with sensible fallbacks. */
struct retry_policy retry_policy_default(void);

/* Classify (rc, status, body) from an http_sse_post call.
 * Returns 1 if retrying is safe and likely to help, 0 otherwise.
 *
 *   status == 0           — transport error (DNS, connect, reset): retry
 *   status 2xx, rc != 0   — mid-stream drop after some events: don't retry
 *   status 408/429/5xx    — transient server-side: retry
 *   anything else (4xx)   — permanent (auth, bad request): don't retry
 *
 * 429 has a body-aware override: when `body` parses as JSON and the
 * `error.type` or `error.code` field marks the limit as terminal
 * (Codex `usage_limit_reached` / `usage_not_included`, OpenAI
 * `insufficient_quota`, `quota_exceeded`), this returns 0 even though
 * the status alone would say retry. Per-minute rate caps
 * (`rate_limit_exceeded`) still retry. `body` may be NULL. */
int retry_should_attempt(int rc, long status, const char *body);

/* Exponential backoff with bounded jitter. `attempt` is 0-based — the
 * delay returned is the wait *between* attempt N and attempt N+1.
 * Jitter is +/-25% derived from monotonic_ms() so concurrent agents
 * don't synchronize on the exact same delay. Result is clamped to
 * [base_delay_ms, max_delay_ms]. */
long retry_delay_ms(const struct retry_policy *pol, int attempt);

/* Sleep for `ms` milliseconds in 100ms slices, polling `tick` between
 * each slice so a user Esc cancels promptly. Returns 1 if the tick
 * fired (cancelled), 0 if the full duration elapsed.
 *
 * When `tick` is NULL the call is a plain nanosleep — useful in tests
 * where there's no cancellation source. */
int retry_sleep_with_tick(long ms, http_tick_cb tick, void *user);

#endif /* HAX_RETRY_H */
