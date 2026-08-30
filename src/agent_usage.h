/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_USAGE_H
#define HAX_AGENT_USAGE_H

#include <stddef.h>

#include "provider.h"

#define AGENT_STATS_MAX_SEGMENTS 3
#define AGENT_STATS_SEGMENT_LEN  64

/* Smallest cost the transcript and /session still display. */
#define COST_DISPLAY_MIN 0.00005

/* Use decimal k/M suffixes for token counts — tokens are specified and billed in decimal
 * multiples, unlike bytes. Negative values produce "?". */
void format_tokens(char *out, size_t out_size, long tokens);
/* Include the usage percentage when context_limit is positive; negative context_tokens means
 * unknown usage ("? / 256k", no percentage). */
void format_context(char *out, size_t out_size, long context_tokens, long context_limit);

/* Format turn duration, context use, and session spend in display order. Negative token/time
 * values and nonpositive spend are omitted. Returns the number of populated segments. */
int agent_format_stats_segments(char segments[][AGENT_STATS_SEGMENT_LEN], long context_tokens,
                                long context_limit, long elapsed_ms, double session_spend,
                                int spend_estimated);

struct catalog_split;
struct spend_record;

/* Zero-initialize before use and release with agent_spend_free. */
struct spend_totals {
    struct spend_record *records;
    size_t count;
    size_t capacity;
};

/* Record one response. Live rates are snapshotted so later model switches cannot reprice it; the
 * catalog identity is retained as a lazy fallback when rates are not yet available. */
void agent_spend_account(struct spend_totals *totals, const struct stream_usage *usage,
                         const struct provider *provider, const char *model);

/* Return session spend in USD. `estimated` is set when any response lacks reported cost,
 * including responses that cannot currently be priced. */
double agent_spend_total(const struct spend_totals *totals, int *estimated);

/* Return true if a pending catalog fetch could make the spend estimate more complete. */
int agent_spend_has_unpriced(const struct spend_totals *totals);

/* Sum estimated per-category costs into `split`; return true if any record could be priced.
 * Providers do not report category-level charges, so this split is inexact even when the total is
 * exact. */
int agent_spend_split(const struct spend_totals *totals, struct catalog_split *split);

void agent_spend_free(struct spend_totals *totals);

/* Return input tokens billed at the ordinary input rate. Cache writes are subtracted only when
 * their rate replaces, rather than surcharges, ordinary input billing. */
long agent_usage_uncached_input(const struct stream_usage *usage, const struct provider *provider,
                                const char *model);

/* Return true when the response reports tokens or cost. */
int agent_usage_is_reported(const struct stream_usage *usage);

/* Add `extra` into `sum` field by field, leaving fields neither side reports unreported. The
 * exact cost survives only when it covers every merged token: a side reporting tokens without
 * cost drops the aggregate to the estimated path. */
void agent_usage_add(struct stream_usage *sum, const struct stream_usage *extra);

/* Build an owned transcript footer payload. Returns NULL when neither usage nor duration was
 * reported. The total uses reported cost when available; category costs are always estimates. */
struct turn_usage *agent_turn_usage_new(const struct stream_usage *usage, long elapsed_ms,
                                        const struct provider *provider, const char *model);

#endif /* HAX_AGENT_USAGE_H */
