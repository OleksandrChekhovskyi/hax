/* SPDX-License-Identifier: MIT */
#include "openai_events.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

void openai_events_init(struct openai_events *s, stream_cb cb, void *user)
{
    memset(s, 0, sizeof(*s));
    s->cb = cb;
    s->user = user;
    s->pending_usage.input_tokens = -1;
    s->pending_usage.output_tokens = -1;
    s->pending_usage.cached_tokens = -1;
}

void openai_events_free(struct openai_events *s)
{
    for (size_t i = 0; i < s->n_tools; i++) {
        free(s->tools[i].id);
        free(s->tools[i].name);
        buf_free(&s->tools[i].pending_args);
    }
    free(s->tools);
    s->tools = NULL;
    s->n_tools = s->cap_tools = 0;
    free(s->finish_reason);
    s->finish_reason = NULL;
}

static struct openai_tool_track *track_find(struct openai_events *s, int index)
{
    for (size_t i = 0; i < s->n_tools; i++) {
        if (s->tools[i].index == index)
            return &s->tools[i];
    }
    return NULL;
}

static struct openai_tool_track *track_get_or_add(struct openai_events *s, int index)
{
    struct openai_tool_track *t = track_find(s, index);
    if (t)
        return t;
    if (s->n_tools == s->cap_tools) {
        size_t cap = s->cap_tools ? s->cap_tools * 2 : 4;
        s->tools = xrealloc(s->tools, cap * sizeof(*s->tools));
        s->cap_tools = cap;
    }
    t = &s->tools[s->n_tools++];
    memset(t, 0, sizeof(*t));
    t->index = index;
    return t;
}

static int emit(struct openai_events *s, const struct stream_event *ev)
{
    return s->cb(ev, s->user);
}

/* Emit EV_TOOL_CALL_START for a track once the name is known and we haven't
 * emitted yet. OpenAI typically delivers id + name on the first tool_call
 * delta, but some compat backends stagger them and some omit `id` entirely;
 * synthesize `call_<index>` in that case so the call still dispatches (the
 * id is just a round-trip token the server echoes back as tool_call_id). If
 * argument bytes arrived before the metadata, flush them as a single DELTA
 * right after START so the downstream turn layer sees the whole args string. */
static void maybe_emit_start(struct openai_events *s, struct openai_tool_track *t)
{
    if (t->start_emitted || !t->name)
        return;
    if (!t->id)
        t->id = xasprintf("call_%d", t->index);
    struct stream_event ev = {
        .kind = EV_TOOL_CALL_START,
        .u.tool_call_start = {.id = t->id, .name = t->name},
    };
    emit(s, &ev);
    t->start_emitted = 1;
    if (t->pending_args.len > 0) {
        struct stream_event dev = {
            .kind = EV_TOOL_CALL_DELTA,
            .u.tool_call_delta = {.id = t->id, .args_delta = t->pending_args.data},
        };
        emit(s, &dev);
        buf_reset(&t->pending_args);
    }
}

static void handle_text_delta(struct openai_events *s, const char *text)
{
    if (!text || !*text)
        return;
    struct stream_event ev = {
        .kind = EV_TEXT_DELTA,
        .u.text_delta = {.text = text},
    };
    emit(s, &ev);
}

static void handle_tool_call_delta(struct openai_events *s, json_t *tc)
{
    /* `index` is required per OpenAI spec, but some compat servers omit it
     * when streaming a single tool call. Default to 0 so we still dispatch
     * the call (multi-parallel-without-index is pathological and rare). */
    json_t *jindex = json_object_get(tc, "index");
    int index = json_is_integer(jindex) ? (int)json_integer_value(jindex) : 0;
    struct openai_tool_track *t = track_get_or_add(s, index);

    const char *id = json_string_value(json_object_get(tc, "id"));
    if (id && !t->id)
        t->id = xstrdup(id);

    json_t *fn = json_object_get(tc, "function");
    const char *name = fn ? json_string_value(json_object_get(fn, "name")) : NULL;
    if (name && !t->name)
        t->name = xstrdup(name);

    maybe_emit_start(s, t);

    const char *args = fn ? json_string_value(json_object_get(fn, "arguments")) : NULL;
    if (args && *args) {
        if (t->start_emitted) {
            struct stream_event ev = {
                .kind = EV_TOOL_CALL_DELTA,
                .u.tool_call_delta = {.id = t->id, .args_delta = args},
            };
            emit(s, &ev);
        } else {
            /* Metadata hasn't fully arrived yet — buffer for later. */
            buf_append_str(&t->pending_args, args);
        }
    }
}

static void end_all_tool_calls(struct openai_events *s)
{
    for (size_t i = 0; i < s->n_tools; i++) {
        struct openai_tool_track *t = &s->tools[i];
        if (!t->start_emitted || t->ended)
            continue;
        struct stream_event ev = {
            .kind = EV_TOOL_CALL_END,
            .u.tool_call_end = {.id = t->id},
        };
        emit(s, &ev);
        t->ended = 1;
    }
}

/* Capture usage from any chunk that carries it. With
 * stream_options.include_usage, OpenAI sends one trailing chunk with empty
 * `choices` and a populated `usage` just before [DONE]; we keep the values
 * around until the deferred EV_DONE fires. cached_tokens lives in
 * prompt_tokens_details — absent on most compat backends, hence the -1
 * "unknown" default. */
static void capture_usage(struct openai_events *s, json_t *root)
{
    json_t *usage = json_object_get(root, "usage");
    if (!json_is_object(usage))
        return;

    json_t *v = json_object_get(usage, "prompt_tokens");
    if (json_is_integer(v))
        s->pending_usage.input_tokens = (long)json_integer_value(v);
    v = json_object_get(usage, "completion_tokens");
    if (json_is_integer(v))
        s->pending_usage.output_tokens = (long)json_integer_value(v);

    json_t *details = json_object_get(usage, "prompt_tokens_details");
    if (json_is_object(details)) {
        v = json_object_get(details, "cached_tokens");
        if (json_is_integer(v))
            s->pending_usage.cached_tokens = (long)json_integer_value(v);
    }
}

static void emit_deferred_done(struct openai_events *s)
{
    struct stream_event ev = {
        .kind = EV_DONE,
        .u.done = {.stop_reason = s->finish_reason ? s->finish_reason : "stop",
                   .usage = s->pending_usage},
    };
    emit(s, &ev);
}

/* finish_reason semantics:
 *   "stop" / "tool_calls" → EV_DONE (deferred; see below)
 *   "length" / "content_filter" → EV_ERROR (truncated; discard partial turn)
 * Everything else is treated as unknown and maps to EV_DONE to avoid hanging.
 *
 * EV_DONE is deferred because under stream_options.include_usage the usage
 * chunk arrives one event AFTER finish_reason. We end tool calls now (the
 * response is logically complete) but hold off on EV_DONE until [DONE] or
 * the SSE transport closes — whichever sees the usage chunk first. */
static void handle_finish_reason(struct openai_events *s, const char *reason)
{
    if (s->terminated || s->saw_finish)
        return;

    if (reason && (strcmp(reason, "length") == 0 || strcmp(reason, "content_filter") == 0)) {
        end_all_tool_calls(s);
        s->terminated = 1;
        char *msg = xasprintf("response incomplete: %s", reason);
        struct stream_event ev = {
            .kind = EV_ERROR,
            .u.error = {.message = msg, .http_status = 0},
        };
        emit(s, &ev);
        free(msg);
        return;
    }

    end_all_tool_calls(s);
    s->saw_finish = 1;
    s->finish_reason = xstrdup(reason ? reason : "stop");
}

static void handle_done_sentinel(struct openai_events *s)
{
    if (s->terminated)
        return;
    end_all_tool_calls(s);
    s->terminated = 1;
    emit_deferred_done(s);
}

/* Some backends surface errors as `{"error":{"message":"...","code":...}}`
 * inside the SSE stream rather than via HTTP status. */
static void handle_error_object(struct openai_events *s, json_t *err)
{
    if (s->terminated)
        return;
    s->terminated = 1;
    const char *m = json_string_value(json_object_get(err, "message"));
    struct stream_event ev = {
        .kind = EV_ERROR,
        .u.error = {.message = m ? m : "provider error", .http_status = 0},
    };
    emit(s, &ev);
}

void openai_events_feed(struct openai_events *s, const char *data)
{
    if (!data || !*data)
        return;
    if (strcmp(data, "[DONE]") == 0) {
        handle_done_sentinel(s);
        return;
    }

    json_error_t jerr;
    json_t *root = json_loads(data, 0, &jerr);
    if (!root)
        return;

    json_t *err = json_object_get(root, "error");
    if (json_is_object(err)) {
        handle_error_object(s, err);
        json_decref(root);
        return;
    }

    /* Trailing chunks with stream_options.include_usage carry an empty
     * choices array plus the usage object — capture before the early-out. */
    capture_usage(s, root);

    json_t *choices = json_object_get(root, "choices");
    if (!json_is_array(choices) || json_array_size(choices) == 0) {
        json_decref(root);
        return;
    }
    json_t *choice = json_array_get(choices, 0);
    json_t *delta = json_object_get(choice, "delta");

    if (json_is_object(delta)) {
        /* Reasoning deltas: OpenRouter normalizes to `reasoning`,
         * llama.cpp/DeepSeek emit `reasoning_content` (kept as a compat
         * alias by OpenRouter too). We surface both so the agent can
         * flip the spinner to "thinking..." while CoT is streaming. */
        const char *r = json_string_value(json_object_get(delta, "reasoning"));
        if (!r)
            r = json_string_value(json_object_get(delta, "reasoning_content"));
        if (r && *r) {
            struct stream_event ev = {
                .kind = EV_REASONING_DELTA,
                .u.reasoning_delta = {.text = r},
            };
            emit(s, &ev);
        }

        const char *text = json_string_value(json_object_get(delta, "content"));
        handle_text_delta(s, text);

        json_t *tool_calls = json_object_get(delta, "tool_calls");
        if (json_is_array(tool_calls)) {
            size_t i, n = json_array_size(tool_calls);
            for (i = 0; i < n; i++)
                handle_tool_call_delta(s, json_array_get(tool_calls, i));
        }
    }

    json_t *finish = json_object_get(choice, "finish_reason");
    if (json_is_string(finish))
        handle_finish_reason(s, json_string_value(finish));

    json_decref(root);
}

void openai_events_finalize(struct openai_events *s)
{
    if (s->terminated)
        return;
    s->terminated = 1;
    /* Some backends close the SSE stream after finish_reason without ever
     * sending [DONE] — emit the deferred done now so the agent doesn't
     * mistake a clean close for a truncated stream. */
    if (s->saw_finish) {
        emit_deferred_done(s);
        return;
    }
    struct stream_event ev = {
        .kind = EV_ERROR,
        .u.error = {.message = "stream ended before completion", .http_status = 0},
    };
    emit(s, &ev);
}
