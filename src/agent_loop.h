/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_LOOP_H
#define HAX_AGENT_LOOP_H

#include <stddef.h>

#include "agent_core.h"
#include "provider.h"
#include "turn.h"

/* Continuation engine shared by the frontends: drive one user turn through provider
 * round-trips — stream a response, absorb it into the session, dispatch the requested tools,
 * repeat until the model stops calling tools — including abort repair and finished-task notes
 * at turn seams. Presentation and cancellation come in through hooks; record bookkeeping that
 * stands alone without a stream belongs in agent_core. */

/* One provider stream() call and the state assembled from its events. */
struct agent_loop_turn {
    struct turn assembly;
    struct stream_usage usage;
    /* Summed usage of attempts that died mid-stream and were retried. Kept apart from `usage`
     * so input + output still measures the terminal attempt's context window. */
    struct stream_usage retry_usage;
    /* Owned copy of the terminal event's stream_response, whose strings are only borrowed. */
    char *response_id;
    char *served_model;
    char *route;
    char *error_message;
    long elapsed_ms;
};

/* Run one model turn. session_id is the conversation's stable id for provider affinity (may be
 * NULL). observer receives each event for optional presentation (its return value is ignored);
 * event assembly, terminal usage, errors, and timing are always captured here. tick is the
 * provider's optional wait-loop side channel. */
void agent_loop_turn_run(struct agent_loop_turn *loop_turn, struct agent_session *session,
                         struct provider *provider, const char *session_id, stream_cb observer,
                         void *observer_user, http_tick_cb tick, void *tick_user);
void agent_loop_turn_destroy(struct agent_loop_turn *loop_turn);

/* Billable usage for the whole turn: the terminal attempt plus retried attempts. */
struct stream_usage agent_loop_turn_usage_total(const struct agent_loop_turn *loop_turn);

enum agent_abort_reason {
    AGENT_ABORT_PROVIDER_ERROR,
    AGENT_ABORT_USER_CANCEL,
};

/* Slice of original streamed items absorbed during abort repair. Synthesized tool results and a
 * possible standalone marker follow items_to. */
struct agent_abort_outcome {
    int marker_placed;
    size_t items_from;
    size_t items_to;
};

/* Repair an aborted turn into well-formed, marked history. Truncated reasoning is never kept:
 * replayed as finished state it derails models. A user cancel absorbs everything else, but a turn
 * keeping no text and no completed call leaves history untouched, as does a provider error, which
 * keeps only assistant text — either way a retry re-issues the same request. */
struct agent_abort_outcome agent_loop_turn_absorb_abort(struct agent_session *session,
                                                        struct agent_loop_turn *loop_turn,
                                                        enum agent_abort_reason reason);

struct transcript_log;
struct session_log;

enum agent_loop_tool_action {
    AGENT_LOOP_TOOL_RUN,
    AGENT_LOOP_TOOL_REFUSE,
    AGENT_LOOP_TOOL_SKIP,
};

/* What the frontend's checkpoint hook asks of the loop. ABORT stops now: remaining batch tools
 * are skipped with interrupted results and the run ends AGENT_LOOP_INTERRUPTED. PAUSE is the soft
 * variant: in-flight work (the streamed response, every tool in the batch) still completes, and
 * the run ends AGENT_LOOP_PAUSED at the turn seam with clean, fully paired history — no marker —
 * so the frontend can resume it verbatim. */
enum agent_loop_signal {
    AGENT_LOOP_SIG_NONE = 0,
    AGENT_LOOP_SIG_PAUSE,
    AGENT_LOOP_SIG_ABORT,
};

/* Optional frontend behavior. checkpoint settles and samples cancellation, returning an
 * agent_loop_signal; tool_call must return the matching owned result for the requested action,
 * and when NULL the loop falls back to silent semantic tool execution; compact performs the
 * frontend's compaction transaction when the shared threshold is reached. */
struct agent_loop_hooks {
    void *user;
    stream_cb observe;
    http_tick_cb tick;
    void (*turn_begin)(void *user);
    void (*turn_end)(const struct agent_loop_turn *loop_turn, void *user);
    int (*checkpoint)(void *user);
    void (*tool_seen)(const struct item *call, void *user);
    /* image_input is the resolved provider/model capability for the run context. */
    struct item (*tool_call)(const struct item *call, enum agent_loop_tool_action action,
                             int image_input, void *user);
    void (*compact)(void *user);
    /* A background-task note was appended to history before this turn's request; text is
     * borrowed for the call. Display-only. */
    void (*task_note)(const char *text, void *user);
};

/* Every outcome except COMPLETE leaves an incomplete user turn behind. PAUSED and MAX_TURNS stop
 * at a clean turn seam (all calls paired, no markers), so re-running the loop against the same
 * history continues the turn; INTERRUPTED and PROVIDER_ERROR ran abort repair first. */
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
    /* Streamed items absorbed from the final turn; synthesized repair and usage items are
     * outside this half-open range. */
    size_t final_items_from;
    size_t final_items_to;
    /* Abort repair kept partial output, marked interrupted. A frontend resuming a marked run
     * must speak for the user — history ends mid-story otherwise — where a marker-free stop
     * resumes silently. */
    int abort_marker_placed;
    char *error_message;
};

struct agent_loop_params {
    struct agent_session *session;
    struct provider *provider;
    struct transcript_log *tlog;
    struct session_log *slog;
    int max_turns; /* < 0 means unlimited */
    /* Resuming an incomplete user turn with no new user input: the first round-trip continues
     * the previous seam rather than following a fresh user message, so it owes a turn boundary
     * like a follow-up turn does. */
    int continued;
    struct agent_loop_hooks hooks;
};

/* Continue an already-appended user message through provider turns and tool calls until the
 * model returns without a tool call or the run aborts. Holds the idle-sleep inhibitor for the
 * complete continuation run. */
void agent_loop_run(const struct agent_loop_params *params, struct agent_loop_result *result);
void agent_loop_result_destroy(struct agent_loop_result *result);

#endif /* HAX_AGENT_LOOP_H */
