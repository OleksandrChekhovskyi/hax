/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_STATS_H
#define HAX_AGENT_STATS_H

#include <stddef.h>

#include "catalog.h"

/* Session accounting derived from the recorded conversation rather than tracked alongside it, so
 * a live run, a resumed file, a fork, and an undone branch all report the same numbers for the
 * same record. */

struct agent_session;
struct provider;

#define AGENT_STATS_MAX_TOOLS  8
#define AGENT_STATS_MAX_MODELS 8

/* Token and spend totals over a set of usage records. Token counters sum reported values. */
struct agent_stats_totals {
    long requests; /* usage records, one per request the provider served */
    long input_tokens;
    long output_tokens;
    long cached_tokens;
    long cache_write_tokens;
    long uncached_input_tokens;
    double spend;        /* USD; exact when no record is estimated */
    int spend_estimated; /* some record lacks a reported cost, or could not be priced at all */
    int unpriced;        /* some record has neither a reported cost nor rates to estimate one */
    struct catalog_split split; /* category estimates; valid when split_available */
    int split_available;
};

struct agent_stats_model {
    const char *provider; /* borrowed display labels, spelled as the banner spells them */
    const char *model;
    struct agent_stats_totals totals;
};

struct agent_stats {
    /* The live conversation: what the model sees now. */
    long user_turns;
    long undone_user_turns; /* typed prompts /undo removed; their requests stay in `total` */
    long tool_calls;
    struct {
        const char *name; /* borrowed from the item; NULL marks a free slot */
        long count;
    } tools[AGENT_STATS_MAX_TOOLS];
    long worked_ms;
    long context_tokens; /* newest reported window use in the live context; -1 when unknown */
    struct agent_stats_totals total;                         /* everything this session paid for */
    struct agent_stats_model models[AGENT_STATS_MAX_MODELS]; /* in order of first use */
    size_t n_models;
    /* Context copied by /fork and still live: paid for by the source session. */
    long inherited_user_turns;
    struct agent_stats_totals inherited;
};

#define AGENT_STATS_MAX_SEGMENTS 3
#define AGENT_STATS_SEGMENT_LEN  64

/* Format the user turn's duration, context use, and session spend in display order for the stats
 * line. Negative token/time values and nonpositive spend are omitted. Returns the number of
 * populated segments. */
int agent_format_stats_segments(char segments[][AGENT_STATS_SEGMENT_LEN], long context_tokens,
                                long context_limit, long elapsed_ms, double session_spend,
                                int spend_estimated);

/* Derive the accounting of a session from its items: the live conversation from `from_item` on
 * and the retired items from `from_retired` on, so a run scoped to what it appended passes the
 * counts it started with. Inherited items feed only `inherited`. `provider` may be NULL; when it
 * produced a usage record written before rates were known, its live rates price it. Borrowed
 * strings in `out` stay valid until the session's items change. */
void agent_stats_collect(const struct agent_session *session, size_t from_item, size_t from_retired,
                         const struct provider *provider, struct agent_stats *out);

#endif /* HAX_AGENT_STATS_H */
