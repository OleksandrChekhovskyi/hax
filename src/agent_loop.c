/* SPDX-License-Identifier: MIT */
#include "agent_loop.h"

#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "agent_tool.h"
#include "agent_usage.h"
#include "compact.h"
#include "model_meta.h"
#include "provider.h"
#include "session.h"
#include "tool.h"
#include "transcript.h"
#include "turn.h"
#include "xalloc.h"
#include "system/clock.h"
#include "system/keepawake.h"
#include "tools/task_registry.h"
#include "transport/http.h"

struct loop_turn_sink {
    struct agent_loop_turn *loop_turn;
    stream_cb observer;
    void *observer_user;
};

static void capture_response(struct agent_loop_turn *loop_turn,
                             const struct stream_response *response)
{
    if (!response)
        return;
    if (!loop_turn->response_id && response->id)
        loop_turn->response_id = xstrdup(response->id);
    if (!loop_turn->served_model && response->model)
        loop_turn->served_model = xstrdup(response->model);
    if (!loop_turn->route && response->route)
        loop_turn->route = xstrdup(response->route);
}

/* Borrowed view of the identity captured above, for the turn's usage footer. */
static struct stream_response turn_response(const struct agent_loop_turn *loop_turn)
{
    return (struct stream_response){
        .id = loop_turn->response_id,
        .model = loop_turn->served_model,
        .route = loop_turn->route,
    };
}

static int loop_turn_on_event(const struct stream_event *ev, void *user)
{
    struct loop_turn_sink *sink = user;
    struct agent_loop_turn *loop_turn = sink->loop_turn;

    if (ev->kind == EV_DONE) {
        loop_turn->usage = ev->u.done.usage;
        capture_response(loop_turn, &ev->u.done.response);
    } else if (ev->kind == EV_ERROR) {
        if (!loop_turn->error_message && ev->u.error.message)
            loop_turn->error_message = xstrdup(ev->u.error.message);
        if (ev->u.error.usage)
            loop_turn->usage = *ev->u.error.usage;
        capture_response(loop_turn, ev->u.error.response);
    } else if (ev->kind == EV_RETRY && ev->u.retry.usage) {
        agent_usage_add(&loop_turn->retry_usage, ev->u.retry.usage);
    }

    if (sink->observer)
        sink->observer(ev, sink->observer_user);
    turn_consume(&loop_turn->assembly, ev);
    return 0;
}

void agent_loop_turn_run(struct agent_loop_turn *loop_turn, struct agent_session *session,
                         struct provider *provider, const char *session_id, stream_cb observer,
                         void *observer_user, http_tick_cb tick, void *tick_user)
{
    memset(loop_turn, 0, sizeof(*loop_turn));
    turn_init(&loop_turn->assembly);
    loop_turn->usage = (struct stream_usage){-1, -1, -1, -1, -1, -1};
    loop_turn->retry_usage = (struct stream_usage){-1, -1, -1, -1, -1, -1};

    struct loop_turn_sink sink = {
        .loop_turn = loop_turn,
        .observer = observer,
        .observer_user = observer_user,
    };
    struct context ctx = agent_session_context(session);
    ctx.image_input = model_meta_image_input(provider, session->model);
    ctx.session_id = session_id;
    long started_ms = monotonic_ms();
    provider->stream(provider, &ctx, session->model, loop_turn_on_event, &sink, tick, tick_user);
    loop_turn->elapsed_ms = monotonic_ms() - started_ms;
}

struct stream_usage agent_loop_turn_usage_total(const struct agent_loop_turn *loop_turn)
{
    struct stream_usage total = loop_turn->usage;
    agent_usage_add(&total, &loop_turn->retry_usage);
    return total;
}

void agent_loop_turn_destroy(struct agent_loop_turn *loop_turn)
{
    turn_reset(&loop_turn->assembly);
    free(loop_turn->error_message);
    loop_turn->error_message = NULL;
    free(loop_turn->response_id);
    loop_turn->response_id = NULL;
    free(loop_turn->served_model);
    loop_turn->served_model = NULL;
    free(loop_turn->route);
    loop_turn->route = NULL;
}

/* True when the assembly holds assistant text — flushed items or the open buffer. */
static int assembly_has_text(const struct turn *assembly)
{
    for (size_t i = 0; i < assembly->n_items; i++)
        if (assembly->items[i].kind == ITEM_ASSISTANT_MESSAGE)
            return 1;
    return assembly->has_text;
}

/* True when user-cancel repair keeps anything: assistant text or a completed tool call.
 * Reasoning never counts — repair discards the truncated buffer, and sealed reasoning without
 * text or calls evaporates with the turn. */
static int assembly_survives_cancel(const struct turn *assembly)
{
    for (size_t i = 0; i < assembly->n_items; i++)
        if (assembly->items[i].kind == ITEM_ASSISTANT_MESSAGE ||
            assembly->items[i].kind == ITEM_TOOL_CALL)
            return 1;
    return assembly->has_text;
}

struct agent_abort_outcome agent_loop_turn_absorb_abort(struct agent_session *session,
                                                        struct agent_loop_turn *loop_turn,
                                                        enum agent_abort_reason reason)
{
    struct turn *assembly = &loop_turn->assembly;
    /* Truncated reasoning re-sent as finished state derails models, so no repair replays it.
     * A provider error keeps only assistant text: a call that never ran would also need a
     * fabricated result, and with both dropped a retry re-issues the same request. */
    if (reason == AGENT_ABORT_PROVIDER_ERROR) {
        turn_keep_text(assembly);
    } else {
        turn_discard_reasoning(assembly);
        /* With nothing else streamed the turn evaporates — no items, no marker — as if the
         * request was never sent, so an empty-send resume re-issues it verbatim. */
        if (!assembly_survives_cancel(assembly)) {
            turn_reset(assembly);
            return (struct agent_abort_outcome){
                .items_from = session->n_items,
                .items_to = session->n_items,
            };
        }
    }
    int kept_text = assembly_has_text(assembly);
    int had_partial_text = assembly->has_text;
    turn_flush_text(assembly, had_partial_text ? "\n" INTERRUPT_MARKER : NULL);

    struct agent_absorb_result absorbed = agent_session_absorb(session, assembly);

    int marker_placed = had_partial_text;
    size_t items_from = absorbed.items_from;
    size_t items_to = session->n_items;
    if (had_partial_text) {
        /* The "\n[interrupted]" turn_flush_text appended above is ours, not the model's. Stamp
         * the item that carries it — the last assistant message absorbed — so display strips
         * exactly what we added instead of recognizing it by content (a response can
         * legitimately end on that line). */
        for (size_t i = items_to; i-- > items_from;) {
            if (session->items[i].kind == ITEM_ASSISTANT_MESSAGE) {
                session->items[i].origin = ITEM_ORIGIN_INTERRUPTED;
                break;
            }
        }
    }
    for (size_t i = items_from; i < items_to; i++) {
        if (session->items[i].kind != ITEM_TOOL_CALL)
            continue;
        struct item closed = agent_tool_result_make(&session->items[i], INTERRUPT_MARKER, NULL);
        /* The stream was cut before dispatch reached this call: it never ran, same as one Esc
         * skipped mid-batch. */
        closed.origin = ITEM_ORIGIN_SKIPPED;
        agent_session_append(session, closed);
        marker_placed = 1;
    }

    if (!marker_placed && (reason == AGENT_ABORT_USER_CANCEL || kept_text)) {
        struct item *last_text = NULL;
        for (size_t i = items_to; i-- > items_from;) {
            if (session->items[i].kind == ITEM_ASSISTANT_MESSAGE) {
                last_text = &session->items[i];
                break;
            }
        }
        if (last_text && last_text->text) {
            /* Chat serialization joins adjacent assistant items without a separator, so a
             * standalone marker would glue onto the text it interrupts. */
            char *marked = xasprintf("%s\n%s", last_text->text, INTERRUPT_MARKER);
            free(last_text->text);
            last_text->text = marked;
            last_text->origin = ITEM_ORIGIN_INTERRUPTED;
        } else {
            agent_session_append(session, (struct item){
                                              .kind = ITEM_ASSISTANT_MESSAGE,
                                              .text = xstrdup(INTERRUPT_MARKER),
                                              .origin = ITEM_ORIGIN_INTERRUPTED,
                                          });
        }
        marker_placed = 1;
    }

    return (struct agent_abort_outcome){
        .marker_placed = marker_placed,
        .items_from = items_from,
        .items_to = items_to,
    };
}

static int loop_checkpoint(const struct agent_loop_hooks *hooks)
{
    return hooks->checkpoint ? hooks->checkpoint(hooks->user) : AGENT_LOOP_SIG_NONE;
}

static struct item loop_run_tool(const struct agent_loop_params *params, const struct item *call,
                                 enum agent_loop_tool_action action)
{
    const struct agent_loop_hooks *hooks = &params->hooks;
    /* Resolved per call, not per session: the answer can change under a runtime /model switch
     * and when an async capability probe lands. */
    int image_input = model_meta_image_input(params->provider, params->session->model);

    struct item result;
    if (hooks->tool_call) {
        result = hooks->tool_call(call, action, image_input, hooks->user);
    } else if (action == AGENT_LOOP_TOOL_REFUSE) {
        /* Hookless execution owes the same provenance as frontend-rendered results: the action,
         * not the presentation path, determines whether the call was dispatched. */
        result = agent_tool_result_make(call, REFUSED_RESULT, NULL);
        result.origin = ITEM_ORIGIN_REFUSED;
    } else if (action == AGENT_LOOP_TOOL_SKIP) {
        result = agent_tool_result_make(call, INTERRUPT_MARKER, NULL);
        result.origin = ITEM_ORIGIN_SKIPPED;
    } else {
        struct agent_tool_call prepared;
        agent_tool_call_init(&prepared, call);
        struct tool_run_ctx run_ctx = {.image_input = image_input};
        char *output = agent_tool_call_run(&prepared, &run_ctx);
        result = agent_tool_result_make(call, output, &run_ctx);
        free(output);
        agent_tool_call_destroy(&prepared);
    }

    /* Enforce the aggregate image budget at ingestion — the window excludes `result`, which the
     * caller appends next. Dropping the just-read image (rather than degrading older ones at
     * serialization) keeps prior requests byte-stable, so the provider prefix cache survives.
     * The budget is what a request may carry, so a compacted-away image no longer counts. */
    struct context window = agent_session_context(params->session);
    agent_tool_result_enforce_image_budget(window.items, window.n_items, &result);
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
    struct stream_usage usage = agent_loop_turn_usage_total(loop_turn);
    if (!aborted || agent_usage_is_reported(&usage)) {
        struct stream_response response = turn_response(loop_turn);
        agent_session_add_turn_usage(params->session, params->provider, &usage,
                                     loop_turn->elapsed_ms, &response);
    }
}

static void loop_flush(const struct agent_loop_params *params)
{
    agent_flush_logs(params->tlog, params->slog, params->session->items, params->session->n_items);
}

static void loop_run_active(const struct agent_loop_params *params,
                            struct agent_loop_result *result)
{
    struct agent_session *session = params->session;
    const struct agent_loop_hooks *hooks = &params->hooks;
    memset(result, 0, sizeof(*result));
    /* Falling out of the loop is the only max-turn path; every terminal provider/cancel outcome
     * returns from its branch below. */
    result->outcome = AGENT_LOOP_MAX_TURNS;
    result->last_context_tokens = -1;

    for (int turn_n = 0; params->max_turns < 0 || turn_n < params->max_turns; turn_n++) {
        /* The first boundary arrived with the user message — except on a continued run, whose
         * first turn extends the previous seam. Follow-up turns owe their own. Either way it is
         * appended lazily — just before this turn's items land in history — so a turn that
         * leaves nothing behind (a pause pre-empting a still-prefilling request, a provider
         * failure before any output) doesn't leave a dangling empty turn header in the
         * transcript. Boundaries are inert to providers, so context built without one is
         * unaffected. */
        int owes_boundary = turn_n > 0 || params->continued;

        /* Deliver finished-task notes before building this turn's request, so the model hears
         * about completions at the earliest seam: between tool batches mid-turn, and with the
         * user's next prompt at the start of a run. */
        char *note = task_collect_notes();
        if (note) {
            agent_session_append(session, (struct item){
                                              .kind = ITEM_USER_MESSAGE,
                                              .text = note,
                                              .origin = ITEM_ORIGIN_TASK_NOTE,
                                          });
            /* The model hears the note with this request, so a session resumed after a
             * mid-request crash must already contain it. */
            loop_flush(params);
            if (hooks->task_note)
                hooks->task_note(note, hooks->user);
        }

        if (hooks->turn_begin)
            hooks->turn_begin(hooks->user);

        struct agent_loop_turn loop_turn;
        agent_loop_turn_run(&loop_turn, session, params->provider, session_log_id(params->slog),
                            hooks->observe, hooks->user, hooks->tick, hooks->user);
        result->turns++;
        /* Account the request before branching: errored and interrupted turns still reached the
         * provider and may carry billable usage. */
        if (hooks->turn_end)
            hooks->turn_end(&loop_turn, hooks->user);

        long turn_context = -1;
        if (loop_turn.usage.input_tokens >= 0 && loop_turn.usage.output_tokens >= 0) {
            turn_context = loop_turn.usage.input_tokens + loop_turn.usage.output_tokens;
            result->last_context_tokens = turn_context;
        }

        if (loop_turn.assembly.state == TURN_FAILED) {
            /* Provider failure wins over a simultaneous frontend cancel: it supplies the
             * diagnostic. The boundary is owed exactly when something lands after it — the
             * partial text repair keeps and/or a billable-usage footer — so it neither dangles
             * empty nor lets the footer read as part of the preceding turn. */
            struct stream_usage failed_usage = agent_loop_turn_usage_total(&loop_turn);
            if (owes_boundary &&
                (assembly_has_text(&loop_turn.assembly) || agent_usage_is_reported(&failed_usage)))
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

        /* Sample cancellation after a clean stream but before absorption or dispatch, so a late
         * Esc cannot launch tools or another turn. A pause request is only noted: the batch it
         * precedes still runs in full, and the stop lands at this turn's seam. */
        int pause_pending = 0;
        int sig = loop_checkpoint(hooks);
        if (sig == AGENT_LOOP_SIG_ABORT) {
            /* The boundary is owed exactly when the cancel leaves something after it — repaired
             * items with their marker, or a billable-usage footer. An evaporating turn (nothing
             * but truncated thinking) leaves no trace at all. */
            struct stream_usage cancelled_usage = agent_loop_turn_usage_total(&loop_turn);
            if (owes_boundary && (assembly_survives_cancel(&loop_turn.assembly) ||
                                  agent_usage_is_reported(&cancelled_usage)))
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

        /* A pre-empted request — the pause-cancelling tick aborted the stream before any
         * content, so no terminal event and nothing assembled — leaves no trace; the resume
         * re-sends it. Anything a turn does leave (items, a footer — even just banked usage
         * from a pause landing mid-retry) owes the boundary. */
        int paused_empty = pause_pending && loop_turn.assembly.state == TURN_STREAMING &&
                           loop_turn.assembly.n_items == 0;
        struct stream_usage banked_usage = agent_loop_turn_usage_total(&loop_turn);
        if (owes_boundary && (!paused_empty || agent_usage_is_reported(&banked_usage)))
            agent_session_add_boundary(session);
        struct agent_absorb_result absorbed = agent_session_absorb(session, &loop_turn.assembly);
        /* Freeze the streamed slice before appending results: tool results must never be
         * mistaken for more calls, and frontends need the final turn range without its
         * synthesized results or usage footer. */
        size_t response_to = session->n_items;
        loop_observe_tools(params, absorbed.items_from, response_to);
        result->final_items_from = absorbed.items_from;
        result->final_items_to = response_to;

        if (!absorbed.had_tool_call) {
            /* A tool-free response completes the user turn — unless the pause pre-empted the
             * request (paused_empty above): an empty cancelled turn is nothing to complete, so
             * pause here and let a resume re-send the request. */
            loop_add_usage(params, &loop_turn, paused_empty);
            loop_flush(params);
            agent_loop_turn_destroy(&loop_turn);
            result->outcome = paused_empty ? AGENT_LOOP_PAUSED : AGENT_LOOP_COMPLETE;
            return;
        }

        /* Every streamed tool call gets exactly one result. Disabled tools are refused rather
         * than executed; once cancellation is observed, the remaining batch is paired with
         * interrupted results. A pause request never skips: the whole batch runs and the stop
         * waits for the seam. */
        for (size_t i = absorbed.items_from; i < response_to; i++) {
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
            agent_session_append(session, tool_result);
        }

        /* The final checkpoint catches cancellation raised by the last tool, when there is no
         * next call to sample it. Place the marker before the footer so usage remains the turn's
         * trailing item. */
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

        /* The seam is the pause point: calls, results, and footer are durable and no marker is
         * owed — history reads as a finished provider turn a later run can continue verbatim.
         * Deliberately before the compact seam: a pause returns control without launching new
         * work, and an over-threshold seam is instead compacted by the run that continues it,
         * before its first request (the frontend's re-entry path owns that check). */
        if (pause_pending) {
            result->outcome = AGENT_LOOP_PAUSED;
            return;
        }

        /* Compact only at a continuation seam, after calls/results/footer are durable and before
         * the next boundary. Cancellation during the frontend transaction must stop continuation
         * against old history. */
        if (compact_should_auto(turn_context,
                                model_meta_context(params->provider, session->model)) &&
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
