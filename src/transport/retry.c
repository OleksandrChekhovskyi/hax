/* SPDX-License-Identifier: MIT */
#include "transport/retry.h"

#include <jansson.h>
#include <limits.h>
#include <strings.h>
#include <time.h>

#include "config.h"
#include "system/clock.h"
#include "transport/http.h"

#define MAX_DELAY_MS   30000
#define SLEEP_SLICE_MS 100

static const char *const TERMINAL_429_CODES[] = {
    "usage_limit_reached", "usage_not_included", "insufficient_quota",
    "quota_exceeded",      "GoUsageLimitError",  NULL,
};

struct retry_policy retry_policy_default(void)
{
    int additional_retries = config_int("http.max_retries");
    long base_delay_ms = config_duration_ms("http.retry_base");
    long idle_timeout_ms = config_duration_ms("http.idle_timeout");

    struct retry_policy policy = {
        .max_attempts = additional_retries + 1,
        .base_delay_ms = base_delay_ms,
        .max_delay_ms = MAX_DELAY_MS,
        /* libcurl accepts whole seconds; preserve a configured sub-second timeout. */
        .idle_timeout_s = idle_timeout_ms / 1000 + (idle_timeout_ms % 1000 ? 1 : 0),
    };
    return policy;
}

static int is_terminal_429_code(const char *code)
{
    if (!code)
        return 0;

    for (const char *const *terminal = TERMINAL_429_CODES; *terminal; terminal++) {
        if (strcasecmp(code, *terminal) == 0)
            return 1;
    }
    return 0;
}

static int has_terminal_429_error(const char *body)
{
    if (!body || !*body)
        return 0;

    json_t *root = json_loads(body, 0, NULL);
    if (!root)
        return 0;

    int terminal = 0;
    json_t *error = json_object_get(root, "error");
    if (json_is_object(error)) {
        json_t *type = json_object_get(error, "type");
        terminal = json_is_string(type) && is_terminal_429_code(json_string_value(type));
        if (!terminal) {
            json_t *code = json_object_get(error, "code");
            terminal = json_is_string(code) && is_terminal_429_code(json_string_value(code));
        }
    }
    json_decref(root);
    return terminal;
}

int retry_should_attempt(int result, long status, const char *body)
{
    if (result == 0 && status >= 200 && status < 300)
        return 0;
    if (status == 0)
        return 1;
    if (status >= 200 && status < 300)
        return 0;
    if (status == 408)
        return 1;
    if (status == 429)
        return !has_terminal_429_error(body);
    return status >= 500 && status <= 599;
}

long retry_delay_ms(const struct retry_policy *policy, int attempt)
{
    if (policy->base_delay_ms <= 0 || policy->max_delay_ms <= 0)
        return 0;
    if (attempt < 0)
        attempt = 0;

    long delay_ms = policy->base_delay_ms;
    if (delay_ms > policy->max_delay_ms)
        delay_ms = policy->max_delay_ms;
    for (int i = 0; i < attempt; i++) {
        if (delay_ms > policy->max_delay_ms / 2) {
            delay_ms = policy->max_delay_ms;
            break;
        }
        delay_ms *= 2;
    }

    long jitter_percent = 75 + monotonic_ms() % 51;
    if (delay_ms > LONG_MAX / jitter_percent)
        delay_ms = policy->max_delay_ms;
    else
        delay_ms = delay_ms * jitter_percent / 100;
    if (delay_ms > policy->max_delay_ms)
        delay_ms = policy->max_delay_ms;
    return delay_ms;
}

int retry_sleep_with_tick(long delay_ms, http_tick_cb tick, void *user)
{
    if (delay_ms <= 0)
        return tick && tick(user);

    long deadline = monotonic_ms() + delay_ms;
    while (1) {
        if (tick && tick(user))
            return 1;

        long remaining_ms = deadline - monotonic_ms();
        if (remaining_ms <= 0)
            return 0;
        if (remaining_ms > SLEEP_SLICE_MS)
            remaining_ms = SLEEP_SLICE_MS;

        struct timespec duration = {
            .tv_sec = remaining_ms / 1000,
            .tv_nsec = (remaining_ms % 1000) * 1000000L,
        };
        nanosleep(&duration, NULL);
    }
}
