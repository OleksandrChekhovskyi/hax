/* SPDX-License-Identifier: MIT */
#ifndef HAX_COMPACT_H
#define HAX_COMPACT_H

#include "provider.h" /* stream_cb, http_tick_cb */

struct agent_session;
struct session_log;
struct transcript_log;

/*
 * Conversation compaction: replace a long history with a model-generated
 * structured summary so the session can keep going without overflowing the
 * context window.
 *
 * The shared mechanism lives here: prompt/seed policy, auto-trigger and
 * context-window resolution, summary retries, attempt footers, history
 * replacement, and log rotation. Frontends supply only event accounting,
 * transport ticks, cancellation, and presentation around compact_run.
 */

enum compact_outcome {
    COMPACT_COMPLETE,
    COMPACT_NO_PROVIDER,
    COMPACT_NO_MODEL,
    COMPACT_EMPTY,
    COMPACT_CANCELLED,
    COMPACT_PROVIDER_ERROR,
    COMPACT_NO_SUMMARY,
};

/* Frontends may observe events for usage accounting, provide a transport tick,
 * and sample cancellation after summarization but before history is replaced.
 * The observe callback's return value is ignored. */
struct compact_hooks {
    void *user;
    stream_cb observe;
    http_tick_cb tick;
    int (*cancelled)(void *user);
};

struct compact_params {
    struct agent_session *session;
    struct provider *provider;
    struct session_log *slog;
    struct transcript_log *tlog;
    const char *instructions;
    struct compact_hooks hooks;
};

struct compact_result {
    enum compact_outcome outcome;
    int attempts;
    char *error_message;
};

/* Run the complete compaction transaction: summarize, reject stray tool calls,
 * preserve footers from every billed attempt, replace history on success, and
 * rotate/flush both logs at the canonical commit points. */
void compact_run(const struct compact_params *params, struct compact_result *result);
void compact_result_destroy(struct compact_result *result);

/* Whether auto-compaction is enabled (config key compact.auto). Manual
 * /compact ignores this — it always runs. */
int compact_auto_enabled(void);

/* Pure threshold test: is `ctx_tokens` at or past `pct`% of `limit`?
 * Returns 0 when limit <= 0 (unknown window) or ctx_tokens < 0. */
int compact_over_threshold(long ctx_tokens, long limit, int pct);

/* Decide whether auto-compaction should fire after a (stream) turn, given
 * the last reported context token count and the resolved context-window
 * limit. Honors compact.auto and compact.threshold (percent). Returns 0 when
 * disabled, when the limit is unknown (<= 0), or when ctx is below the
 * threshold. */
int compact_should_auto(long ctx_tokens, long limit);

/* Resolve the context-window size used for the auto-compaction threshold and
 * the per-user-turn "%" display: HAX_CONTEXT_LIMIT (manual override) wins,
 * else the provider's auto-detected probe value, else the model-catalog
 * entry for `model` (providers with a catalog_id but no probe of their own —
 * openai, anthropic), else 0 ("unknown" — both the percentage and
 * auto-compaction are then disabled). `model` may be NULL (skips the
 * catalog tier). */
long compact_context_limit(const struct provider *p, const char *model);

#endif /* HAX_COMPACT_H */
