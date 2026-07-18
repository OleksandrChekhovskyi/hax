/* SPDX-License-Identifier: MIT */
#include "agent_loop.h"

#include <stdlib.h>
#include <string.h>

#include "agent_tool.h"
#include "compact.h"
#include "session.h"
#include "transcript.h"
#include "util.h"
#include "system/keepawake.h"

struct loop_turn_sink {
    struct agent_loop_turn *loop_turn;
    stream_cb observer;
    void *observer_user;
};

static int loop_turn_on_event(const struct stream_event *ev, void *user)
{
    struct loop_turn_sink *sink = user;
    struct agent_loop_turn *loop_turn = sink->loop_turn;

    if (ev->kind == EV_DONE) {
        loop_turn->usage = ev->u.done.usage;
        loop_turn->done = 1;
    } else if (ev->kind == EV_ERROR) {
        if (!loop_turn->error_message && ev->u.error.message)
            loop_turn->error_message = xstrdup(ev->u.error.message);
        if (ev->u.error.usage)
            loop_turn->usage = *ev->u.error.usage;
    }

    if (sink->observer)
        sink->observer(ev, sink->observer_user);
    turn_on_event(ev, &loop_turn->assembly);
    return 0;
}

void agent_loop_turn_run(struct agent_loop_turn *loop_turn, struct agent_session *session,
                         struct provider *provider, stream_cb observer, void *observer_user,
                         http_tick_cb tick, void *tick_user)
{
    memset(loop_turn, 0, sizeof(*loop_turn));
    turn_init(&loop_turn->assembly);
    loop_turn->usage = (struct stream_usage){-1, -1, -1, -1, -1, -1};

    struct loop_turn_sink sink = {
        .loop_turn = loop_turn,
        .observer = observer,
        .observer_user = observer_user,
    };
    struct context ctx = agent_session_context(session);
    long started_ms = monotonic_ms();
    provider->stream(provider, &ctx, session->model, loop_turn_on_event, &sink, tick, tick_user);
    loop_turn->elapsed_ms = monotonic_ms() - started_ms;
}

void agent_loop_turn_destroy(struct agent_loop_turn *loop_turn)
{
    turn_reset(&loop_turn->assembly);
    free(loop_turn->error_message);
    loop_turn->error_message = NULL;
}

int agent_loop_turn_has_state(const struct agent_loop_turn *loop_turn)
{
    const struct turn *assembly = &loop_turn->assembly;
    return assembly->in_text || assembly->in_reasoning || assembly->n_pending > 0 ||
           assembly->n_items > 0;
}

struct agent_abort_outcome agent_loop_turn_absorb_abort(struct agent_session *session,
                                                        struct agent_loop_turn *loop_turn,
                                                        enum agent_abort_reason reason)
{
    struct turn *assembly = &loop_turn->assembly;
    int had_state = agent_loop_turn_has_state(loop_turn);
    int had_partial_text = assembly->in_text;
    turn_flush_reasoning(assembly);
    turn_flush_text(assembly, had_partial_text ? "\n" INTERRUPT_MARKER : NULL);

    size_t items_from;
    agent_session_absorb(session, assembly, &items_from, NULL);

    int marker_placed = had_partial_text;
    size_t items_to = session->n_items;
    for (size_t i = items_from; i < items_to; i++) {
        if (session->items[i].kind != ITEM_TOOL_CALL)
            continue;
        items_append(&session->items, &session->n_items, &session->cap_items,
                     agent_tool_result_make(&session->items[i], INTERRUPT_MARKER));
        marker_placed = 1;
    }

    if (!marker_placed && (reason == AGENT_ABORT_USER_CANCEL || had_state)) {
        items_append(&session->items, &session->n_items, &session->cap_items,
                     (struct item){
                         .kind = ITEM_ASSISTANT_MESSAGE,
                         .text = xstrdup(INTERRUPT_MARKER),
                     });
        marker_placed = 1;
    }

    return (struct agent_abort_outcome){
        .had_state = had_state,
        .marker_placed = marker_placed,
        .items_from = items_from,
        .items_to = items_to,
    };
}

void agent_loop_flush_logs(struct transcript_log *tlog, struct session_log *slog,
                           const struct item *items, size_t n_items)
{
    transcript_log_append(tlog, items, n_items);
    session_log_append(slog, items, n_items);
}

static int loop_checkpoint(const struct agent_loop_hooks *hooks)
{
    return hooks->checkpoint ? hooks->checkpoint(hooks->user) : AGENT_LOOP_SIG_NONE;
}

static struct item loop_run_tool(const struct agent_loop_params *params, const struct item *call,
                                 enum agent_loop_tool_action action)
{
    const struct agent_loop_hooks *hooks = &params->hooks;
    if (hooks->tool_call)
        return hooks->tool_call(call, action, hooks->user);

    if (action == AGENT_LOOP_TOOL_REFUSE)
        return agent_tool_result_make(call, "error: tool calls are disabled in this session");
    if (action == AGENT_LOOP_TOOL_SKIP)
        return agent_tool_result_make(call, INTERRUPT_MARKER);

    struct agent_tool_call tool_call;
    agent_tool_call_init(&tool_call, call);
    char *output = agent_tool_call_run(&tool_call, NULL, NULL);
    struct item result = agent_tool_result_make(call, output);
    free(output);
    agent_tool_call_destroy(&tool_call);
    return result;
}

static void loop_observe_tools(const struct agent_loop_params *params, size_t from, size_t to)
{
    if (!params->hooks.tool_seen)
        return;
    for (size_t i = from; i < to; i++)
        if (params->session->items[i].kind == ITEM_TOOL_CALL)
            params->hooks.tool_seen(&params->session->items[i], params->hooks.user);
}

static void loop_add_usage(const struct agent_loop_params *params,
                           const struct agent_loop_turn *loop_turn, int aborted)
{
    if (!aborted || usage_reported(&loop_turn->usage))
        agent_session_add_turn_usage(params->session, params->provider, &loop_turn->usage,
                                     loop_turn->elapsed_ms);
}

static void loop_flush(const struct agent_loop_params *params)
{
    agent_loop_flush_logs(params->tlog, params->slog, params->session->items,
                          params->session->n_items);
}

static void loop_run_active(const struct agent_loop_params *params,
                            struct agent_loop_result *result)
{
    struct agent_session *session = params->session;
    const struct agent_loop_hooks *hooks = &params->hooks;
    memset(result, 0, sizeof(*result));
    /* Falling out of the loop is the only max-turn path; every terminal
     * provider/cancel outcome returns from its branch below. */
    result->outcome = AGENT_LOOP_MAX_TURNS;
    result->last_context_tokens = -1;

    for (int turn_n = 0; params->max_turns < 0 || turn_n < params->max_turns; turn_n++) {
        /* The first boundary arrived with the user message — except on a
         * continued run, whose first turn extends the previous seam.
         * Follow-up turns owe their own. Either way it is appended lazily —
         * just before this turn's items land in history — so a turn that
         * leaves nothing behind (a pause pre-empting a still-prefilling
         * request, a provider failure before any output) doesn't leave a
         * dangling empty turn header in the transcript. Boundaries are
         * inert to providers, so context built without one is unaffected. */
        int owes_boundary = turn_n > 0 || params->continued;
        if (hooks->turn_begin)
            hooks->turn_begin(hooks->user);

        struct agent_loop_turn loop_turn;
        agent_loop_turn_run(&loop_turn, session, params->provider, hooks->observe, hooks->user,
                            hooks->tick, hooks->user);
        result->turns++;
        /* Account the request before branching: errored and interrupted turns
         * still reached the provider and may carry billable usage. */
        if (hooks->turn_end)
            hooks->turn_end(&loop_turn, hooks->user);

        long turn_context = -1;
        if (loop_turn.usage.input_tokens >= 0 && loop_turn.usage.output_tokens >= 0) {
            turn_context = loop_turn.usage.input_tokens + loop_turn.usage.output_tokens;
            result->last_context_tokens = turn_context;
        }

        if (loop_turn.assembly.error) {
            /* Provider failure wins over a simultaneous frontend cancel. It
             * supplies the diagnostic, while abort repair preserves any
             * partial output and closes completed tool calls. Repair appends
             * items only when the stream produced state, but an EV_ERROR can
             * also carry billable usage without content — its retained
             * footer owes the boundary too, or it would read as part of the
             * preceding turn. Only a no-state, no-usage failure leaves
             * history (and the owed boundary) untouched. */
            if (owes_boundary &&
                (agent_loop_turn_has_state(&loop_turn) || usage_reported(&loop_turn.usage)))
                agent_session_add_boundary(session);
            struct agent_abort_outcome abort =
                agent_loop_turn_absorb_abort(session, &loop_turn, AGENT_ABORT_PROVIDER_ERROR);
            loop_observe_tools(params, abort.items_from, abort.items_to);
            result->final_items_from = abort.items_from;
            result->final_items_to = abort.items_to;
            result->abort_marker_placed = abort.marker_placed;
            result->error_message =
                loop_turn.error_message ? xstrdup(loop_turn.error_message) : NULL;
            loop_add_usage(params, &loop_turn, 1);
            loop_flush(params);
            agent_loop_turn_destroy(&loop_turn);
            result->outcome = AGENT_LOOP_PROVIDER_ERROR;
            return;
        }

        /* Sample cancellation after a clean stream but before absorption or
         * dispatch, so a late Esc cannot launch tools or another turn. A
         * pause request is only noted: the batch it precedes still runs in
         * full, and the stop lands at this turn's seam. */
        int pause_pending = 0;
        int sig = loop_checkpoint(hooks);
        if (sig == AGENT_LOOP_SIG_ABORT) {
            /* A user cancel always leaves a marker, so the boundary is
             * always owed. */
            if (owes_boundary)
                agent_session_add_boundary(session);
            struct agent_abort_outcome abort =
                agent_loop_turn_absorb_abort(session, &loop_turn, AGENT_ABORT_USER_CANCEL);
            loop_observe_tools(params, abort.items_from, abort.items_to);
            result->final_items_from = abort.items_from;
            result->final_items_to = abort.items_to;
            result->abort_marker_placed = abort.marker_placed;
            loop_add_usage(params, &loop_turn, 1);
            loop_flush(params);
            agent_loop_turn_destroy(&loop_turn);
            result->outcome = AGENT_LOOP_INTERRUPTED;
            return;
        }
        if (sig == AGENT_LOOP_SIG_PAUSE)
            pause_pending = 1;

        /* A pre-empted request — the pause-cancelling tick aborted the stream
         * before any content, so no EV_DONE and nothing assembled. It leaves
         * no trace: no boundary, no footer, outcome PAUSED. Every other turn
         * leaves items and/or a footer (even a legitimately empty completed
         * response gets its duration footer), so its boundary is owed. */
        int paused_empty = pause_pending && !loop_turn.done && loop_turn.assembly.n_items == 0;
        if (owes_boundary && !paused_empty)
            agent_session_add_boundary(session);
        size_t items_from;
        int had_tool_call;
        agent_session_absorb(session, &loop_turn.assembly, &items_from, &had_tool_call);
        /* Freeze the streamed slice before appending results: tool results must
         * never be mistaken for more calls, and frontends need the final turn
         * range without its synthesized results or usage footer. */
        size_t response_to = session->n_items;
        loop_observe_tools(params, items_from, response_to);
        result->final_items_from = items_from;
        result->final_items_to = response_to;

        if (!had_tool_call) {
            /* A tool-free response completes the user turn — unless the
             * pause pre-empted the request (paused_empty above): an empty
             * cancelled turn is nothing to complete, so pause here and let
             * a resume re-send the request. */
            loop_add_usage(params, &loop_turn, paused_empty);
            loop_flush(params);
            agent_loop_turn_destroy(&loop_turn);
            result->outcome = paused_empty ? AGENT_LOOP_PAUSED : AGENT_LOOP_COMPLETE;
            return;
        }

        /* Every streamed tool call gets exactly one result. Disabled tools are
         * refused rather than executed; once cancellation is observed, the
         * remaining batch is paired with interrupted results. A pause request
         * never skips: the whole batch runs and the stop waits for the seam. */
        for (size_t i = items_from; i < response_to; i++) {
            if (session->items[i].kind != ITEM_TOOL_CALL)
                continue;
            sig = loop_checkpoint(hooks);
            if (sig == AGENT_LOOP_SIG_PAUSE)
                pause_pending = 1;
            enum agent_loop_tool_action action = AGENT_LOOP_TOOL_RUN;
            if (session->n_tools == 0)
                action = AGENT_LOOP_TOOL_REFUSE;
            else if (sig == AGENT_LOOP_SIG_ABORT)
                action = AGENT_LOOP_TOOL_SKIP;
            struct item tool_result = loop_run_tool(params, &session->items[i], action);
            items_append(&session->items, &session->n_items, &session->cap_items, tool_result);
        }

        /* The final checkpoint catches cancellation raised by the last tool,
         * when there is no next call to sample it. Place the marker before the
         * footer so usage remains the turn's trailing item. */
        sig = loop_checkpoint(hooks);
        if (sig == AGENT_LOOP_SIG_ABORT)
            agent_session_mark_interrupt(session);
        loop_add_usage(params, &loop_turn, 0);
        loop_flush(params);
        agent_loop_turn_destroy(&loop_turn);

        if (sig == AGENT_LOOP_SIG_ABORT) {
            result->outcome = AGENT_LOOP_INTERRUPTED;
            result->abort_marker_placed = 1;
            return;
        }
        if (sig == AGENT_LOOP_SIG_PAUSE)
            pause_pending = 1;

        /* The seam is the pause point: calls, results, and footer are
         * durable and no marker is owed — history reads as a finished
         * provider turn a later run can continue verbatim. Deliberately
         * before the compact seam: a pause returns control without
         * launching new work, and an over-threshold seam is instead
         * compacted by the run that continues it, before its first
         * request (the frontend's re-entry path owns that check). */
        if (pause_pending) {
            result->outcome = AGENT_LOOP_PAUSED;
            return;
        }

        /* Compact only at a continuation seam, after calls/results/footer are
         * durable and before the next boundary. Cancellation during the
         * frontend transaction must stop continuation against old history. */
        if (compact_should_auto(turn_context,
                                compact_context_limit(params->provider, session->model)) &&
            hooks->compact) {
            hooks->compact(hooks->user);
            sig = loop_checkpoint(hooks);
            if (sig == AGENT_LOOP_SIG_ABORT) {
                agent_session_mark_interrupt(session);
                loop_flush(params);
                result->outcome = AGENT_LOOP_INTERRUPTED;
                result->abort_marker_placed = 1;
                return;
            }
            if (sig == AGENT_LOOP_SIG_PAUSE) {
                result->outcome = AGENT_LOOP_PAUSED;
                return;
            }
        }
    }
}

void agent_loop_run(const struct agent_loop_params *params, struct agent_loop_result *result)
{
    keepawake_acquire();
    loop_run_active(params, result);
    keepawake_release();
}

void agent_loop_result_destroy(struct agent_loop_result *result)
{
    free(result->error_message);
    result->error_message = NULL;
}
