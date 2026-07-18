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
    /* EV_DONE arrived — the stream completed normally. Distinguishes a
     * legitimately empty response (completed, owes its boundary and usage
     * footer) from a stream cancelled before producing anything (a soft
     * interrupt pre-empting a prefilling request — leaves no trace). */
    int done;
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

/* True when the turn's stream produced anything at all — finished items,
 * open text/reasoning, or a pending tool call. Abort repair appends items
 * exactly when this holds (plus the always-marked user-cancel case), so the
 * loop uses it to decide whether a follow-up turn's boundary is owed. */
int agent_loop_turn_has_state(const struct agent_loop_turn *loop_turn);

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

/* What the frontend's checkpoint hook asks of the loop. ABORT stops now:
 * remaining batch tools are skipped with interrupted results and the run
 * ends AGENT_LOOP_INTERRUPTED. PAUSE is the soft variant: in-flight work
 * (the streamed response, every tool in the batch) still completes, and
 * the run ends AGENT_LOOP_PAUSED at the turn seam with clean, fully
 * paired history — no marker — so the frontend can resume it verbatim. */
enum agent_loop_signal {
    AGENT_LOOP_SIG_NONE = 0,
    AGENT_LOOP_SIG_PAUSE,
    AGENT_LOOP_SIG_ABORT,
};

/* Optional frontend behavior. checkpoint settles and samples cancellation,
 * returning an agent_loop_signal; tool_call must return the matching owned
 * result for the requested action. A NULL tool_call uses silent semantic
 * tool execution. compact performs the frontend's compaction transaction
 * when the shared threshold is reached. */
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

/* Every outcome except COMPLETE leaves an incomplete user turn behind.
 * PAUSED and MAX_TURNS stop at a clean turn seam (all calls paired, no
 * markers), so re-running the loop against the same history continues the
 * turn; INTERRUPTED and PROVIDER_ERROR ran abort repair first. */
enum agent_loop_outcome {
    AGENT_LOOP_COMPLETE,
    AGENT_LOOP_PROVIDER_ERROR,
    AGENT_LOOP_INTERRUPTED,
    AGENT_LOOP_PAUSED,
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
    /* Abort repair left interrupt markers in history (always for a user
     * cancel; for a provider error only when the stream had produced
     * state). A frontend resuming such a run must speak for the user —
     * history ends mid-story otherwise — where a marker-free stop resumes
     * silently. */
    int abort_marker_placed;
    char *error_message;
};

struct agent_loop_params {
    struct agent_session *session;
    struct provider *provider;
    struct transcript_log *tlog;
    struct session_log *slog;
    int max_turns; /* < 0 means unlimited */
    /* Resuming an incomplete user turn with no new user input: the first
     * round-trip continues the previous seam rather than following a fresh
     * user message, so it owes a turn boundary like a follow-up turn does
     * (appended lazily, only once the turn leaves items — an eager one
     * would dangle as an empty transcript turn if this request too is
     * pre-empted or fails before output). */
    int continued;
    struct agent_loop_hooks hooks;
};

/* Continue an already-appended user message through provider turns and tool
 * calls until the model returns without a tool call or the run aborts. Holds
 * the idle-sleep inhibitor for the complete continuation run. */
void agent_loop_run(const struct agent_loop_params *params, struct agent_loop_result *result);
void agent_loop_result_destroy(struct agent_loop_result *result);

#endif /* HAX_AGENT_LOOP_H */
