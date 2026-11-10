/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_USAGE_H
#define HAX_AGENT_USAGE_H

#include <stddef.h>

#include "provider.h"

/* Accounting for one provider response: usage arithmetic, the attempts one stream() call
 * served, and the priced usage record appended to the conversation. Conversation totals are
 * agent_stats; display formatting is text/fmt. */

#define ATTEMPT_LOG_MAX 8

/* One request the provider served within a stream() call. */
struct attempt {
    struct stream_usage usage;
    long elapsed_ms;
    /* Owned; a stream_response only borrows its strings. */
    char *response_id;
    char *served_model;
    char *route;
};

/* Usage of every served request in one stream() call: each retried attempt that reported
 * usage, then the terminal response. Initialize with attempt_log_init and release with
 * attempt_log_free. */
struct attempt_log {
    struct attempt attempts[ATTEMPT_LOG_MAX];
    size_t count;
    long attempt_started_ms;
};

void attempt_log_init(struct attempt_log *log);

/* Record the attempt that just ended; `response` may be NULL. Once the log is full, usage folds
 * into the last entry so nothing billable is dropped. */
void attempt_log_record(struct attempt_log *log, const struct stream_usage *usage,
                        const struct stream_response *response);

/* Record a retried attempt; the next attempt's timing starts after the `delay_ms` backoff.
 * Unreported usage records nothing: the provider served no request. */
void attempt_log_retry(struct attempt_log *log, const struct stream_usage *usage, long delay_ms);

/* True when any recorded attempt reported tokens or cost. */
int attempt_log_has_usage(const struct attempt_log *log);

void attempt_log_free(struct attempt_log *log);

/* Return true when the response reports tokens or cost. */
int agent_usage_is_reported(const struct stream_usage *usage);

/* Add `extra` into `sum` field by field, leaving fields neither side reports unreported. The
 * exact cost survives only when it covers every merged token: a side reporting tokens without
 * cost drops the aggregate to the estimated path. */
void agent_usage_add(struct stream_usage *sum, const struct stream_usage *extra);

/* Build an owned usage record payload. Returns NULL when neither usage nor duration was
 * reported. The total uses reported cost when available; category costs are always estimates. */
struct turn_usage *agent_turn_usage_new(const struct stream_usage *usage, long elapsed_ms,
                                        const struct provider *provider, const char *model);

#endif /* HAX_AGENT_USAGE_H */
