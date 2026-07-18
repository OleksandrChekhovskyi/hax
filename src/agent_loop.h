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

struct transcript_log;
struct session_log;

void agent_loop_flush_logs(struct transcript_log *tlog, struct session_log *slog,
                           const struct item *items, size_t n_items);

enum agent_loop_tool_action {
    AGENT_LOOP_TOOL_RUN,
    AGENT_LOOP_TOOL_REFUSE,
    AGENT_LOOP_TOOL_SKIP,
};

/* Optional frontend behavior. checkpoint settles and samples cancellation;
 * tool_call must return the matching owned result for the requested action.
 * A NULL tool_call uses silent semantic tool execution. compact performs the
 * frontend's compaction transaction when the shared threshold is reached. */
struct agent_loop_hooks {
    void *user;
    stream_cb observe;
    http_tick_cb tick;
    void (*turn_begin)(void *user);
    void (*turn_end)(const struct agent_loop_turn *loop_turn, void *user);
    int (*checkpoint)(void *user);
    void (*tool_seen)(const struct item *call, void *user);
    struct item (*tool_call)(const struct item *call, enum agent_loop_tool_action action,
                             void *user);
    void (*compact)(void *user);
};

enum agent_loop_outcome {
    AGENT_LOOP_COMPLETE,
    AGENT_LOOP_PROVIDER_ERROR,
    AGENT_LOOP_INTERRUPTED,
    AGENT_LOOP_MAX_TURNS,
};

struct agent_loop_result {
    enum agent_loop_outcome outcome;
    int turns;
    long last_context_tokens;
    /* Streamed items absorbed from the final turn; synthesized repair and
     * usage items are outside this half-open range. */
    size_t final_items_from;
    size_t final_items_to;
    char *error_message;
};

struct agent_loop_params {
    struct agent_session *session;
    struct provider *provider;
    struct transcript_log *tlog;
    struct session_log *slog;
    int max_turns; /* < 0 means unlimited */
    struct agent_loop_hooks hooks;
};

/* Continue an already-appended user message through provider turns and tool
 * calls until the model returns without a tool call or the run aborts. */
void agent_loop_run(const struct agent_loop_params *params, struct agent_loop_result *result);
void agent_loop_result_destroy(struct agent_loop_result *result);

#endif /* HAX_AGENT_LOOP_H */
