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
    buf_free(&t->reasoning_buf);

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

/* Commit accumulated reasoning_content text as an ITEM_REASONING. Same
 * non-NULL guarantee as flush_text. Placed before the assistant text /
 * tool-call items it precedes so build_messages can attach it as
 * reasoning_content on that assistant message. */
static void flush_reasoning(struct turn *t)
{
    if (!t->in_reasoning)
        return;
    struct item it = {
        .kind = ITEM_REASONING,
        .reasoning_text = t->reasoning_buf.data ? buf_steal(&t->reasoning_buf) : xstrdup(""),
    };
    items_append(t, it);
    t->in_reasoning = 0;
}

void turn_flush_text(struct turn *t, const char *suffix)
{
    if (!t->in_text)
        return;
    if (suffix && *suffix)
        buf_append_str(&t->text_buf, suffix);
    flush_text(t);
}

void turn_flush_reasoning(struct turn *t)
{
    flush_reasoning(t);
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
        /* First visible content closes the preceding reasoning block so the
         * ITEM_REASONING lands before this assistant message. */
        flush_reasoning(t);
        buf_append_str(&t->text_buf, ev->u.text_delta.text);
        t->in_text = 1;
        break;
    case EV_TOOL_CALL_START:
        flush_reasoning(t);
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
    case EV_REASONING_DELTA: {
        /* Accumulate CoT text so it can be round-tripped to the model as
         * reasoning_content (see flush_reasoning / build_messages). Some
         * models (notably Qwen3 via llama.cpp) degrade and emit tool calls
         * inside the reasoning channel when their prior reasoning isn't fed
         * back; preserving it here is what prevents that. Skip NULL/empty
         * deltas: provider.h allows state-only reasoning events that only
         * nudge the UI spinner and carry no text to store. */
        const char *rt = ev->u.reasoning_delta.text;
        if (rt && *rt) {
            buf_append_str(&t->reasoning_buf, rt);
            t->in_reasoning = 1;
        }
        break;
    }
    case EV_RETRY:
    case EV_PROGRESS:
        /* UX-only signals — nothing to commit to history. EV_RETRY just
         * tells the agent to update its spinner; EV_PROGRESS drives
         * the prefill % indicator. The eventual outcome (success or
         * EV_ERROR) drives state. */
        break;
    case EV_REASONING_ITEM: {
        /* Server typically emits the reasoning item before the assistant
         * text/tool-call output items, but flush any in-flight text just
         * in case so wire order is preserved on the round-trip. */
        flush_text(t);
        /* Codex streams the human-readable reasoning as EV_REASONING_DELTA
         * (for the spinner) and finalizes the round-trip form as this
         * structured item. Carry both on one item: the buffered plaintext
         * for the transcript, the opaque JSON for the next request. The
         * plaintext is NOT re-sent (codex.c round-trips reasoning_json). */
        char *rtext = t->in_reasoning ? buf_steal(&t->reasoning_buf) : NULL;
        t->in_reasoning = 0;
        struct item it = {
            .kind = ITEM_REASONING,
            .reasoning_json = xstrdup(ev->u.reasoning_item.json),
            .reasoning_text = rtext,
        };
        items_append(t, it);
        break;
    }
    case EV_DONE:
        /* Commit reasoning before text so a reasoning-only turn (the
         * leaked-tool-call case) still carries its CoT into history. */
        flush_reasoning(t);
        flush_text(t);
        break;
    case EV_ERROR:
        /* Don't flush here. The agent's error handler mirrors the
         * Esc-interrupt path: it tags any in-flight text with an
         * [interrupted] marker before flushing, so a "continue"
         * follow-up turn carries history of what was already streamed.
         * Calling flush_text now would commit the text without the
         * marker and clear t->in_text, leaving the agent unable to
         * tell partial-text from no-text. */
        t->error = 1;
        break;
    }
    return 0;
}
