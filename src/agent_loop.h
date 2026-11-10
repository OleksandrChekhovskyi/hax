/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_LOOP_H
#define HAX_AGENT_LOOP_H

#include <stddef.h>

#include "agent_core.h"
#include "agent_stream.h"
#include "provider.h"

/* Continuation engine shared by the frontends: drive one user turn through provider
 * round-trips — stream a response, absorb it into the session, dispatch the requested tools,
 * repeat until the model stops calling tools — including abort repair and finished-task notes
 * at turn seams. Presentation and cancellation come in through hooks; record bookkeeping that
 * stands alone without a stream belongs in agent_core. */

/* Initialize `loop_turn` and stream one model turn over the session's model-visible context, as
 * agent_stream_request(). session_id is the conversation's stable id for provider affinity (may
 * be NULL). Release with agent_stream_destroy(). */
void agent_loop_turn_run(struct agent_stream *loop_turn, struct agent_session *session,
                         struct provider *provider, const char *session_id, stream_cb observer,
                         void *observer_user, agent_signal_cb poll, void *poll_user);

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
                                                        struct agent_stream *loop_turn,
                                                        enum agent_abort_reason reason);

struct transcript_log;
struct session_log;

enum agent_loop_tool_action {
    AGENT_LOOP_TOOL_RUN,
    AGENT_LOOP_TOOL_REFUSE,
    AGENT_LOOP_TOOL_SKIP,
};

/* Optional frontend behavior; each hook receives `user`. */
struct agent_loop_hooks {
    void *user;
    /* Sees each streamed event for presentation; its return value is ignored. */
    stream_cb observe;
    /* Sampled while a response streams. A signal cuts the response when agent_signal_stops()
     * says so: an abort at any point, a pause only before the response has produced anything. */
    agent_signal_cb poll;
    void (*turn_begin)(void *user);
    /* Settles and samples the stop request at the loop's decision points. ABORT stops now: tools
     * not yet run get interrupted results and the run ends AGENT_LOOP_INTERRUPTED. PAUSE lets the
     * tool batch and a compaction under way finish, then ends the run AGENT_LOOP_PAUSED at the
     * turn seam with clean, fully paired history — no marker — so a later run can continue it
     * verbatim. */
    agent_signal_cb checkpoint;
    /* Sees each tool call the loop records in history. */
    void (*tool_seen)(const struct item *call, void *user);
    /* Returns the owned result for `call` under `action`; image_input is the resolved
     * provider/model capability for the run context. When NULL, the loop makes every result
     * itself, without presentation. */
    struct item (*tool_call)(const struct item *call, enum agent_loop_tool_action action,
                             int image_input, void *user);
    /* Runs the frontend's compaction transaction at a continuation seam where compact_due()
     * holds; returns whether it appended a seed. */
    int (*compact)(void *user);
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
    /* The newest window use a request of this run reported; -1 when none did or a compaction
     * superseded it. */
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
    int max_turns; /* <= 0 means unlimited */
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
