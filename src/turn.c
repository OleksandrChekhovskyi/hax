/* SPDX-License-Identifier: MIT */
#include "turn.h"

#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "provider.h"
#include "util.h"

struct pending_tool_call {
    char *id;
    char *name;
    struct buf arguments;
};

void turn_init(struct turn *turn)
{
    memset(turn, 0, sizeof(*turn));
}

static void pending_tool_call_free(struct pending_tool_call *call)
{
    free(call->id);
    free(call->name);
    buf_free(&call->arguments);
}

void turn_reset(struct turn *turn)
{
    for (size_t i = 0; i < turn->n_pending_calls; i++)
        pending_tool_call_free(&turn->pending_calls[i]);
    free(turn->pending_calls);

    buf_free(&turn->text);
    buf_free(&turn->reasoning);

    for (size_t i = 0; i < turn->n_items; i++)
        item_free(&turn->items[i]);
    free(turn->items);

    turn_init(turn);
}

struct item *turn_take_items(struct turn *turn, size_t *out_count)
{
    struct item *items = turn->items;
    if (out_count)
        *out_count = turn->n_items;
    turn->items = NULL;
    turn->n_items = 0;
    turn->cap_items = 0;
    return items;
}

static void append_item(struct turn *turn, struct item item)
{
    if (turn->n_items == turn->cap_items) {
        size_t capacity = turn->cap_items ? turn->cap_items * 2 : 16;
        turn->items = xrealloc(turn->items, capacity * sizeof(*turn->items));
        turn->cap_items = capacity;
    }
    turn->items[turn->n_items++] = item;
}

static void flush_text(struct turn *turn)
{
    if (!turn->has_text)
        return;

    append_item(turn, (struct item){
                          .kind = ITEM_ASSISTANT_MESSAGE,
                          .text = buf_steal(&turn->text),
                      });
    turn->has_text = 0;
}

static void flush_reasoning(struct turn *turn)
{
    if (!turn->has_reasoning)
        return;

    append_item(turn, (struct item){
                          .kind = ITEM_REASONING,
                          .reasoning_text = buf_steal(&turn->reasoning),
                      });
    turn->has_reasoning = 0;
}

void turn_flush_text(struct turn *turn, const char *suffix)
{
    if (!turn->has_text)
        return;
    if (suffix && *suffix)
        buf_append_str(&turn->text, suffix);
    flush_text(turn);
}

void turn_discard_reasoning(struct turn *turn)
{
    buf_free(&turn->reasoning);
    turn->has_reasoning = 0;
}

void turn_keep_text(struct turn *turn)
{
    turn_discard_reasoning(turn);

    size_t kept = 0;
    for (size_t i = 0; i < turn->n_items; i++) {
        if (turn->items[i].kind == ITEM_ASSISTANT_MESSAGE)
            turn->items[kept++] = turn->items[i];
        else
            item_free(&turn->items[i]);
    }
    turn->n_items = kept;
}

static struct pending_tool_call *find_pending_call(struct turn *turn, const char *id)
{
    if (!id)
        return NULL;
    for (size_t i = 0; i < turn->n_pending_calls; i++) {
        if (turn->pending_calls[i].id && strcmp(turn->pending_calls[i].id, id) == 0)
            return &turn->pending_calls[i];
    }
    return NULL;
}

static void start_tool_call(struct turn *turn, const char *id, const char *name)
{
    if (turn->n_pending_calls == turn->cap_pending_calls) {
        size_t capacity = turn->cap_pending_calls ? turn->cap_pending_calls * 2 : 4;
        turn->pending_calls =
            xrealloc(turn->pending_calls, capacity * sizeof(*turn->pending_calls));
        turn->cap_pending_calls = capacity;
    }

    turn->pending_calls[turn->n_pending_calls++] = (struct pending_tool_call){
        .id = xstrdup(id),
        .name = xstrdup(name),
    };
}

static void finish_tool_call(struct turn *turn, const char *id)
{
    struct pending_tool_call *call = find_pending_call(turn, id);
    if (!call)
        return;

    append_item(turn, (struct item){
                          .kind = ITEM_TOOL_CALL,
                          .call_id = call->id,
                          .tool_name = call->name,
                          .tool_arguments_json = buf_steal(&call->arguments),
                      });
    call->id = NULL;
    call->name = NULL;

    size_t index = (size_t)(call - turn->pending_calls);
    size_t remaining = turn->n_pending_calls - index - 1;
    if (remaining > 0)
        memmove(call, call + 1, remaining * sizeof(*call));
    turn->n_pending_calls--;
}

void turn_consume(struct turn *turn, const struct stream_event *event)
{
    if (turn->state != TURN_STREAMING)
        return;

    switch (event->kind) {
    case EV_TEXT_DELTA:
        flush_reasoning(turn);
        buf_append_str(&turn->text, event->u.text_delta.text);
        turn->has_text = 1;
        break;
    case EV_TOOL_CALL_START:
        flush_reasoning(turn);
        flush_text(turn);
        start_tool_call(turn, event->u.tool_call_start.id, event->u.tool_call_start.name);
        break;
    case EV_TOOL_CALL_DELTA: {
        struct pending_tool_call *call = find_pending_call(turn, event->u.tool_call_delta.id);
        if (call)
            buf_append_str(&call->arguments, event->u.tool_call_delta.args_delta);
        break;
    }
    case EV_TOOL_CALL_END:
        finish_tool_call(turn, event->u.tool_call_end.id);
        break;
    case EV_REASONING_DELTA: {
        const char *text = event->u.reasoning_delta.text;
        if (text && *text) {
            /* Some providers require prior reasoning text on the next request. */
            buf_append_str(&turn->reasoning, text);
            turn->has_reasoning = 1;
        }
        break;
    }
    case EV_REASONING_ITEM:
        /* Opaque state and its preceding display text form one reasoning item. */
        flush_text(turn);
        append_item(turn,
                    (struct item){
                        .kind = ITEM_REASONING,
                        .reasoning_json = xstrdup(event->u.reasoning_item.json),
                        .reasoning_text = turn->has_reasoning ? buf_steal(&turn->reasoning) : NULL,
                    });
        turn->has_reasoning = 0;
        break;
    case EV_RETRY:
        /* The attempt died mid-stream; the retry re-streams the whole response. */
        turn_reset(turn);
        break;
    case EV_PROGRESS:
        break;
    case EV_DONE:
        flush_reasoning(turn);
        flush_text(turn);
        turn->state = TURN_DONE;
        break;
    case EV_ERROR:
        /* Abort repair must tag partial text before it becomes a history item. */
        turn->state = TURN_FAILED;
        break;
    }
}
