/* SPDX-License-Identifier: MIT */
#include "anthropic_events.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

void anthropic_events_init(struct anthropic_events *s, stream_cb cb, void *user)
{
    memset(s, 0, sizeof(*s));
    s->cb = cb;
    s->user = user;
    s->pending_usage.input_tokens = -1;
    s->pending_usage.output_tokens = -1;
    s->pending_usage.cached_tokens = -1;
}

void anthropic_events_free(struct anthropic_events *s)
{
    for (size_t i = 0; i < s->n_blocks; i++) {
        free(s->blocks[i].tool_id);
        free(s->blocks[i].tool_name);
        free(s->blocks[i].redacted_data);
        buf_free(&s->blocks[i].thinking);
        buf_free(&s->blocks[i].signature);
    }
    free(s->blocks);
    s->blocks = NULL;
    s->n_blocks = s->cap_blocks = 0;
    free(s->stop_reason);
    s->stop_reason = NULL;
}

static int emit(struct anthropic_events *s, const struct stream_event *ev)
{
    return s->cb(ev, s->user);
}

static struct anthropic_block_track *block_find(struct anthropic_events *s, int index)
{
    for (size_t i = 0; i < s->n_blocks; i++) {
        if (s->blocks[i].index == index)
            return &s->blocks[i];
    }
    return NULL;
}

static struct anthropic_block_track *block_add(struct anthropic_events *s, int index)
{
    if (s->n_blocks == s->cap_blocks) {
        size_t cap = s->cap_blocks ? s->cap_blocks * 2 : 4;
        s->blocks = xrealloc(s->blocks, cap * sizeof(*s->blocks));
        s->cap_blocks = cap;
    }
    struct anthropic_block_track *t = &s->blocks[s->n_blocks++];
    memset(t, 0, sizeof(*t));
    t->index = index;
    buf_init(&t->thinking);
    buf_init(&t->signature);
    return t;
}

/* A thinking/redacted block is opening: signal reasoning activity with a
 * state-only EV_REASONING_DELTA (empty text) so the "thinking..." spinner
 * lights up even when display:"omitted" suppresses thinking_delta text. */
static void emit_reasoning_state(struct anthropic_events *s)
{
    struct stream_event ev = {
        .kind = EV_REASONING_DELTA,
        .u.reasoning_delta = {.text = ""},
    };
    emit(s, &ev);
}

static void handle_content_block_start(struct anthropic_events *s, json_t *root)
{
    json_t *jindex = json_object_get(root, "index");
    int index = json_is_integer(jindex) ? (int)json_integer_value(jindex) : 0;
    json_t *cb = json_object_get(root, "content_block");
    const char *type = cb ? json_string_value(json_object_get(cb, "type")) : NULL;
    if (!type)
        return;

    struct anthropic_block_track *t = block_find(s, index);
    if (!t)
        t = block_add(s, index);

    if (strcmp(type, "text") == 0) {
        t->kind = ANTHROPIC_BLK_TEXT;
    } else if (strcmp(type, "thinking") == 0) {
        t->kind = ANTHROPIC_BLK_THINKING;
        emit_reasoning_state(s);
    } else if (strcmp(type, "redacted_thinking") == 0) {
        t->kind = ANTHROPIC_BLK_REDACTED;
        const char *data = json_string_value(json_object_get(cb, "data"));
        if (data)
            t->redacted_data = xstrdup(data);
        emit_reasoning_state(s);
    } else if (strcmp(type, "tool_use") == 0) {
        t->kind = ANTHROPIC_BLK_TOOL;
        const char *id = json_string_value(json_object_get(cb, "id"));
        const char *name = json_string_value(json_object_get(cb, "name"));
        t->tool_id = xstrdup(id ? id : "");
        t->tool_name = xstrdup(name ? name : "");
        struct stream_event ev = {
            .kind = EV_TOOL_CALL_START,
            .u.tool_call_start = {.id = t->tool_id, .name = t->tool_name},
        };
        emit(s, &ev);
        t->start_emitted = 1;
    } else {
        t->kind = ANTHROPIC_BLK_OTHER;
    }
}

static void handle_content_block_delta(struct anthropic_events *s, json_t *root)
{
    json_t *jindex = json_object_get(root, "index");
    int index = json_is_integer(jindex) ? (int)json_integer_value(jindex) : 0;
    json_t *delta = json_object_get(root, "delta");
    const char *dtype = delta ? json_string_value(json_object_get(delta, "type")) : NULL;
    if (!dtype)
        return;

    struct anthropic_block_track *t = block_find(s, index);

    if (strcmp(dtype, "text_delta") == 0) {
        const char *text = json_string_value(json_object_get(delta, "text"));
        if (text && *text) {
            struct stream_event ev = {
                .kind = EV_TEXT_DELTA,
                .u.text_delta = {.text = text},
            };
            emit(s, &ev);
        }
    } else if (strcmp(dtype, "thinking_delta") == 0) {
        const char *text = json_string_value(json_object_get(delta, "thinking"));
        if (text && *text) {
            if (t)
                buf_append_str(&t->thinking, text);
            struct stream_event ev = {
                .kind = EV_REASONING_DELTA,
                .u.reasoning_delta = {.text = text},
            };
            emit(s, &ev);
        }
    } else if (strcmp(dtype, "signature_delta") == 0) {
        const char *sig = json_string_value(json_object_get(delta, "signature"));
        if (sig && *sig && t)
            buf_append_str(&t->signature, sig);
    } else if (strcmp(dtype, "input_json_delta") == 0) {
        const char *partial = json_string_value(json_object_get(delta, "partial_json"));
        if (partial && *partial && t && t->start_emitted) {
            struct stream_event ev = {
                .kind = EV_TOOL_CALL_DELTA,
                .u.tool_call_delta = {.id = t->tool_id, .args_delta = partial},
            };
            emit(s, &ev);
        }
    }
}

/* Assemble the round-trip JSON for a finished thinking/redacted block and
 * emit it as EV_REASONING_ITEM. Anthropic requires these blocks be passed
 * back verbatim (with their signature) on the next request when tools are in
 * play — the build path replays this string. The accumulated plaintext is
 * carried separately by the turn layer (from the EV_REASONING_DELTAs) for the
 * transcript; the JSON here is what the model needs back. */
static void emit_reasoning_item(struct anthropic_events *s, struct anthropic_block_track *t)
{
    json_t *obj = NULL;
    if (t->kind == ANTHROPIC_BLK_REDACTED) {
        if (!t->redacted_data)
            return;
        obj = json_pack("{s:s, s:s}", "type", "redacted_thinking", "data", t->redacted_data);
    } else {
        /* A thinking block with neither text nor signature carries nothing
         * worth round-tripping (an empty stub) — skip it. With display
         * "omitted" the text is empty but the signature is present, which is
         * exactly what must travel back, so emit. */
        const char *text = t->thinking.data ? t->thinking.data : "";
        const char *sig = t->signature.data ? t->signature.data : "";
        if (!*text && !*sig)
            return;
        obj = json_pack("{s:s, s:s, s:s}", "type", "thinking", "thinking", text, "signature", sig);
    }
    if (!obj)
        return;
    char *json = json_dumps(obj, JSON_COMPACT);
    json_decref(obj);
    if (!json)
        return;
    struct stream_event ev = {
        .kind = EV_REASONING_ITEM,
        .u.reasoning_item = {.json = json},
    };
    emit(s, &ev);
    free(json);
}

static void handle_content_block_stop(struct anthropic_events *s, json_t *root)
{
    json_t *jindex = json_object_get(root, "index");
    int index = json_is_integer(jindex) ? (int)json_integer_value(jindex) : 0;
    struct anthropic_block_track *t = block_find(s, index);
    if (!t)
        return;

    if (t->kind == ANTHROPIC_BLK_TOOL && t->start_emitted) {
        struct stream_event ev = {
            .kind = EV_TOOL_CALL_END,
            .u.tool_call_end = {.id = t->tool_id},
        };
        emit(s, &ev);
    } else if (t->kind == ANTHROPIC_BLK_THINKING || t->kind == ANTHROPIC_BLK_REDACTED) {
        emit_reasoning_item(s, t);
    }
}

/* Anthropic reports input/cache counts on message_start and the final output
 * count on message_delta. input_tokens is the non-cached prompt portion; the
 * cache_* counts are additional input we sent, so total input = input +
 * cache_read + cache_creation. cached_tokens (the prefix-cache hit) is the
 * cache_read subset. */
static void capture_usage(struct anthropic_events *s, json_t *usage)
{
    if (!json_is_object(usage))
        return;
    long input = -1, cache_read = -1, cache_create = -1, output = -1;
    json_t *v;
    if (json_is_integer(v = json_object_get(usage, "input_tokens")))
        input = (long)json_integer_value(v);
    if (json_is_integer(v = json_object_get(usage, "cache_read_input_tokens")))
        cache_read = (long)json_integer_value(v);
    if (json_is_integer(v = json_object_get(usage, "cache_creation_input_tokens")))
        cache_create = (long)json_integer_value(v);
    if (json_is_integer(v = json_object_get(usage, "output_tokens")))
        output = (long)json_integer_value(v);

    if (input >= 0) {
        long total = input;
        if (cache_read > 0)
            total += cache_read;
        if (cache_create > 0)
            total += cache_create;
        s->pending_usage.input_tokens = total;
    }
    if (cache_read >= 0)
        s->pending_usage.cached_tokens = cache_read;
    if (output >= 0)
        s->pending_usage.output_tokens = output;
}

static void handle_message_start(struct anthropic_events *s, json_t *root)
{
    json_t *msg = json_object_get(root, "message");
    if (msg)
        capture_usage(s, json_object_get(msg, "usage"));
}

static void handle_message_delta(struct anthropic_events *s, json_t *root)
{
    json_t *delta = json_object_get(root, "delta");
    const char *reason = delta ? json_string_value(json_object_get(delta, "stop_reason")) : NULL;
    if (reason) {
        free(s->stop_reason);
        s->stop_reason = xstrdup(reason);
    }
    /* Usage on message_delta is cumulative output; merge it in. */
    capture_usage(s, json_object_get(root, "usage"));
}

static void handle_message_stop(struct anthropic_events *s)
{
    if (s->terminated)
        return;
    s->terminated = 1;

    /* Incomplete terminations surface as errors so the agent tags the partial
     * turn [interrupted] rather than committing it as the final answer:
     *   max_tokens  — output truncated at the cap.
     *   pause_turn  — a long server-tool turn the API paused and expects
     *                 continued by replaying the assistant content. hax drives
     *                 no Anthropic server-side tools, so this isn't expected in
     *                 practice; handle it defensively rather than mistaking a
     *                 paused turn for a finished one.
     * The clean stop reasons (end_turn, tool_use, stop_sequence) fall through
     * to EV_DONE. */
    const char *reason = s->stop_reason;
    if (reason && strcmp(reason, "max_tokens") == 0) {
        struct stream_event ev = {
            .kind = EV_ERROR,
            .u.error = {.message = "response incomplete: max_tokens — raise "
                                   "anthropic.max_tokens or lower the effort level",
                        .http_status = 0},
        };
        emit(s, &ev);
        return;
    }
    if (reason && strcmp(reason, "pause_turn") == 0) {
        struct stream_event ev = {
            .kind = EV_ERROR,
            .u.error = {.message = "response paused before completion (pause_turn)",
                        .http_status = 0},
        };
        emit(s, &ev);
        return;
    }

    struct stream_event ev = {
        .kind = EV_DONE,
        .u.done = {.stop_reason = reason ? reason : "end_turn", .usage = s->pending_usage},
    };
    emit(s, &ev);
}

static void handle_error(struct anthropic_events *s, json_t *root)
{
    if (s->terminated)
        return;
    s->terminated = 1;
    json_t *err = json_object_get(root, "error");
    const char *m = err ? json_string_value(json_object_get(err, "message")) : NULL;
    struct stream_event ev = {
        .kind = EV_ERROR,
        .u.error = {.message = m ? m : "provider error", .http_status = 0},
    };
    emit(s, &ev);
}

void anthropic_events_feed(struct anthropic_events *s, const char *event_name, const char *data)
{
    (void)event_name; /* dispatch on the payload's own "type" */
    if (!data || !*data)
        return;

    json_error_t jerr;
    json_t *root = json_loads(data, 0, &jerr);
    if (!root)
        return;

    const char *type = json_string_value(json_object_get(root, "type"));
    if (!type) {
        json_decref(root);
        return;
    }

    if (strcmp(type, "message_start") == 0)
        handle_message_start(s, root);
    else if (strcmp(type, "content_block_start") == 0)
        handle_content_block_start(s, root);
    else if (strcmp(type, "content_block_delta") == 0)
        handle_content_block_delta(s, root);
    else if (strcmp(type, "content_block_stop") == 0)
        handle_content_block_stop(s, root);
    else if (strcmp(type, "message_delta") == 0)
        handle_message_delta(s, root);
    else if (strcmp(type, "message_stop") == 0)
        handle_message_stop(s);
    else if (strcmp(type, "error") == 0)
        handle_error(s, root);
    /* "ping" and any unknown event types are ignored. */

    json_decref(root);
}

void anthropic_events_finalize(struct anthropic_events *s)
{
    if (s->terminated)
        return;
    s->terminated = 1;
    struct stream_event ev = {
        .kind = EV_ERROR,
        .u.error = {.message = "stream ended before completion", .http_status = 0},
    };
    emit(s, &ev);
}
