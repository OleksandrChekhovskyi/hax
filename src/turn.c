/* SPDX-License-Identifier: MIT */
#include "turn.h"

#include <stdlib.h>
#include <string.h>

void turn_init(struct turn *t)
{
    memset(t, 0, sizeof(*t));
}

void turn_reset(struct turn *t)
{
    for (size_t i = 0; i < t->n_pending; i++) {
        free(t->pending[i].call_id);
        free(t->pending[i].name);
        buf_free(&t->pending[i].args);
    }
    free(t->pending);

    buf_free(&t->text_buf);

    /* Items may still own their strings on the error path; turn_take_items
     * nulls the vector in the success path so this loop is then a no-op. */
    for (size_t i = 0; i < t->n_items; i++)
        item_free(&t->items[i]);
    free(t->items);

    memset(t, 0, sizeof(*t));
}

struct pending_tool *turn_find_pending(struct turn *t, const char *call_id)
{
    if (!call_id)
        return NULL;
    for (size_t i = 0; i < t->n_pending; i++) {
        if (t->pending[i].call_id && strcmp(t->pending[i].call_id, call_id) == 0)
            return &t->pending[i];
    }
    return NULL;
}

struct item *turn_take_items(struct turn *t, size_t *out_n)
{
    struct item *items = t->items;
    if (out_n)
        *out_n = t->n_items;
    t->items = NULL;
    t->n_items = 0;
    t->cap_items = 0;
    return items;
}

static void items_append(struct turn *t, struct item it)
{
    if (t->n_items == t->cap_items) {
        size_t c = t->cap_items ? t->cap_items * 2 : 16;
        t->items = xrealloc(t->items, c * sizeof(struct item));
        t->cap_items = c;
    }
    t->items[t->n_items++] = it;
}

static void flush_text(struct turn *t)
{
    if (!t->in_text)
        return;
    /* buf_steal returns NULL if the buf was never appended to (all deltas
     * were empty strings). Guarantee a non-NULL text string for callers. */
    struct item it = {
        .kind = ITEM_ASSISTANT_MESSAGE,
        .text = t->text_buf.data ? buf_steal(&t->text_buf) : xstrdup(""),
    };
    items_append(t, it);
    t->in_text = 0;
}

static struct pending_tool *pending_push(struct turn *t, const char *call_id, const char *name)
{
    if (t->n_pending == t->cap_pending) {
        size_t c = t->cap_pending ? t->cap_pending * 2 : 4;
        t->pending = xrealloc(t->pending, c * sizeof(*t->pending));
        t->cap_pending = c;
    }
    struct pending_tool *p = &t->pending[t->n_pending++];
    p->call_id = xstrdup(call_id);
    p->name = xstrdup(name);
    buf_init(&p->args);
    return p;
}

int turn_on_event(const struct stream_event *ev, struct turn *t)
{
    switch (ev->kind) {
    case EV_TEXT_DELTA:
        buf_append_str(&t->text_buf, ev->u.text_delta.text);
        t->in_text = 1;
        break;
    case EV_TOOL_CALL_START:
        flush_text(t);
        pending_push(t, ev->u.tool_call_start.id, ev->u.tool_call_start.name);
        break;
    case EV_TOOL_CALL_DELTA: {
        struct pending_tool *p = turn_find_pending(t, ev->u.tool_call_delta.id);
        if (!p)
            break;
        buf_append_str(&p->args, ev->u.tool_call_delta.args_delta);
        break;
    }
    case EV_TOOL_CALL_END: {
        struct pending_tool *p = turn_find_pending(t, ev->u.tool_call_end.id);
        if (!p)
            break;
        /* Same non-NULL guarantee as flush_text: if no delta ever arrived
         * for this tool call, emit an empty args string rather than NULL. */
        struct item it = {
            .kind = ITEM_TOOL_CALL,
            .call_id = xstrdup(p->call_id),
            .tool_name = xstrdup(p->name),
            .tool_arguments_json = p->args.data ? buf_steal(&p->args) : xstrdup(""),
        };
        items_append(t, it);
        break;
    }
    case EV_REASONING_ITEM: {
        /* Server typically emits the reasoning item before the assistant
         * text/tool-call output items, but flush any in-flight text just
         * in case so wire order is preserved on the round-trip. */
        flush_text(t);
        struct item it = {
            .kind = ITEM_REASONING,
            .reasoning_json = xstrdup(ev->u.reasoning_item.json),
        };
        items_append(t, it);
        break;
    }
    case EV_DONE:
        flush_text(t);
        break;
    case EV_ERROR:
        flush_text(t);
        t->error = 1;
        break;
    }
    return 0;
}
