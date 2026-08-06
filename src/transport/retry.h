/* SPDX-License-Identifier: MIT */
#ifndef HAX_TRANSPORT_RETRY_H
#define HAX_TRANSPORT_RETRY_H

#include "transport/http.h"

struct retry_policy {
    int max_attempts; /* includes the initial request; 1 disables retries */
    long base_delay_ms;
    long max_delay_ms;
    long idle_timeout_s; /* 0 disables the streaming low-speed timeout */
};

/* Resolves the HTTP retry and idle-timeout configuration into scalar worker-safe values. */
struct retry_policy retry_policy_default(void);

/* Returns non-zero only for pre-stream transport failures and transient HTTP statuses. A 2xx
 * transport failure is not retryable because callbacks may already have observed partial output.
 * Terminal quota markers in a JSON 429 body also suppress retries. `body` may be NULL. */
int retry_should_attempt(int result, long status, const char *body);

/* Returns the jittered delay after zero-based `attempt`, capped by `max_delay_ms`. */
long retry_delay_ms(const struct retry_policy *policy, int attempt);

/* Waits in short slices so `tick` can cancel promptly. Returns non-zero when cancelled. */
int retry_sleep_with_tick(long delay_ms, http_tick_cb tick, void *user);

#endif /* HAX_TRANSPORT_RETRY_H */
