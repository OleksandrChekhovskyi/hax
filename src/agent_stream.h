/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_STREAM_H
#define HAX_AGENT_STREAM_H

#include "agent_usage.h"
#include "provider.h"
#include "turn.h"

/* One provider request as the agent consumes it: the turn assembled from its events, the usage
 * of every served attempt, and the user's stop request applied while it streams. Every agent
 * request streams through here, so a stop request means the same wherever it lands. */

/* A user's stop request. ABORT stops now. PAUSE is the soft variant: work already producing
 * output finishes, and the caller stops at its next seam. */
enum agent_signal {
    AGENT_SIGNAL_NONE = 0,
    AGENT_SIGNAL_PAUSE,
    AGENT_SIGNAL_ABORT,
};

/* Sample the frontend's current stop request. */
typedef enum agent_signal (*agent_signal_cb)(void *user);

struct agent_stream {
    struct turn assembly;
    /* The latest request's terminal attempt alone, so input + output measures its window. */
    struct stream_usage usage;
    /* Every served attempt, retried ones included, of every request streamed into this record. */
    struct attempt_log attempts;
    char *error_message; /* owned; the error the latest request failed with, or NULL */
};

void agent_stream_init(struct agent_stream *stream);
void agent_stream_destroy(struct agent_stream *stream);

/* Stream one request into `stream`. The assembly, terminal usage, and error restart; attempts
 * accumulate across requests. `observer` (may be NULL) sees every event; its return value is
 * ignored. `poll` (may be NULL) is sampled on every transport tick and may refresh presentation:
 * a signal cuts the request when agent_signal_stops() says so. */
void agent_stream_request(struct agent_stream *stream, struct provider *provider,
                          const struct context *context, const char *model, stream_cb observer,
                          void *observer_user, agent_signal_cb poll, void *poll_user);

/* Whether `signal` stops a request that has assembled `assembly` so far: an abort always, a pause
 * only before any content, so that a pause never discards streamed work. */
int agent_signal_stops(enum agent_signal signal, const struct turn *assembly);

/* Whether the latest request returned without a terminal event because `signal` cut it. */
int agent_stream_cut_by(const struct agent_stream *stream, enum agent_signal signal);

/* True when any attempt reported billable usage. */
int agent_stream_has_usage(const struct agent_stream *stream);

#endif /* HAX_AGENT_STREAM_H */
