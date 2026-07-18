/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_LOOP_H
#define HAX_AGENT_LOOP_H

#include <stddef.h>

#include "agent_core.h"
#include "provider.h"
#include "turn.h"

/* One provider stream() call and the state assembled from its events. */
struct agent_loop_turn {
    struct turn assembly;
    struct stream_usage usage;
    char *error_message;
    long elapsed_ms;
};

/* Run one model turn. observer receives each event for optional
 * presentation (its return value is ignored); event assembly, terminal usage,
 * errors, and timing are always captured here. tick remains the provider's
 * optional wait-loop side channel. */
void agent_loop_turn_run(struct agent_loop_turn *loop_turn, struct agent_session *session,
                         struct provider *provider, stream_cb observer, void *observer_user,
                         http_tick_cb tick, void *tick_user);
void agent_loop_turn_destroy(struct agent_loop_turn *loop_turn);

enum agent_abort_reason {
    AGENT_ABORT_PROVIDER_ERROR,
    AGENT_ABORT_USER_CANCEL,
};

/* Slice of original streamed items absorbed during abort repair. Synthesized
 * tool results and a possible standalone marker follow items_to. */
struct agent_abort_outcome {
    int had_state;
    int marker_placed;
    size_t items_from;
    size_t items_to;
};

/* Preserve partial output from an aborted turn and keep history well formed:
 * tag partial text, absorb completed items, synthesize results for calls that
 * were never run, and add a standalone marker when the abort policy requires
 * one. A provider error with no streamed state adds nothing; user cancellation
 * always leaves an explicit marker. */
struct agent_abort_outcome agent_loop_turn_absorb_abort(struct agent_session *session,
                                                        struct agent_loop_turn *loop_turn,
                                                        enum agent_abort_reason reason);

#endif /* HAX_AGENT_LOOP_H */
