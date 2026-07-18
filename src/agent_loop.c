/* SPDX-License-Identifier: MIT */
#include "agent_loop.h"

#include <stdlib.h>
#include <string.h>

#include "agent_tool.h"
#include "util.h"

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

struct agent_abort_outcome agent_loop_turn_absorb_abort(struct agent_session *session,
                                                        struct agent_loop_turn *loop_turn,
                                                        enum agent_abort_reason reason)
{
    struct turn *assembly = &loop_turn->assembly;
    int had_state = assembly->in_text || assembly->in_reasoning || assembly->n_pending > 0 ||
                    assembly->n_items > 0;
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
