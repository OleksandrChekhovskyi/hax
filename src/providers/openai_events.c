/* SPDX-License-Identifier: MIT */
#include "providers/openai_events.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "provider.h"
#include "util.h"

void openai_events_init(struct openai_events *parser, stream_cb callback, void *callback_user)
{
    memset(parser, 0, sizeof(*parser));
    parser->callback = callback;
    parser->callback_user = callback_user;
    parser->usage.input_tokens = -1;
    parser->usage.output_tokens = -1;
    parser->usage.cached_tokens = -1;
    parser->usage.cache_write_tokens = -1;
    parser->usage.cache_write_1h_tokens = -1;
    parser->usage.cost = -1;
}

void openai_events_free(struct openai_events *parser)
{
    for (size_t i = 0; i < parser->n_tool_calls; i++) {
        free(parser->tool_calls[i].id);
        free(parser->tool_calls[i].name);
        buf_free(&parser->tool_calls[i].arguments_before_start);
    }
    free(parser->tool_calls);
    parser->tool_calls = NULL;
    parser->n_tool_calls = parser->tool_call_capacity = 0;

    free(parser->finish_reason);
    parser->finish_reason = NULL;
    free(parser->finish_error);
    parser->finish_error = NULL;
    free(parser->response_id);
    parser->response_id = NULL;
    free(parser->served_model);
    parser->served_model = NULL;
    free(parser->route);
    parser->route = NULL;
}

static void capture_first_string(char **field, const json_t *root, const char *key)
{
    if (*field)
        return;
    const char *value = json_string_value(json_object_get(root, key));
    if (value && *value)
        *field = xstrdup(value);
}

/* `provider` is OpenRouter's name for the upstream endpoint it routed to; plain OpenAI-compatible
 * servers omit it. */
static void capture_response(struct openai_events *parser, json_t *root)
{
    capture_first_string(&parser->response_id, root, "id");
    capture_first_string(&parser->served_model, root, "model");
    capture_first_string(&parser->route, root, "provider");
}

static struct stream_response response_of(const struct openai_events *parser)
{
    return (struct stream_response){
        .id = parser->response_id,
        .model = parser->served_model,
        .route = parser->route,
    };
}

static struct openai_tool_call *find_tool_call(struct openai_events *parser, int index)
{
    for (size_t i = 0; i < parser->n_tool_calls; i++) {
        if (parser->tool_calls[i].index == index)
            return &parser->tool_calls[i];
    }
    return NULL;
}

static struct openai_tool_call *get_tool_call(struct openai_events *parser, int index)
{
    struct openai_tool_call *call = find_tool_call(parser, index);
    if (call)
        return call;

    if (parser->n_tool_calls == parser->tool_call_capacity) {
        size_t capacity = parser->tool_call_capacity ? parser->tool_call_capacity * 2 : 4;
        parser->tool_calls = xrealloc(parser->tool_calls, capacity * sizeof(*parser->tool_calls));
        parser->tool_call_capacity = capacity;
    }

    call = &parser->tool_calls[parser->n_tool_calls++];
    memset(call, 0, sizeof(*call));
    call->index = index;
    return call;
}

static void emit_event(struct openai_events *parser, const struct stream_event *event)
{
    parser->callback(event, parser->callback_user);
}

static void start_tool_call(struct openai_events *parser, struct openai_tool_call *call)
{
    if (call->started || !call->name)
        return;

    /* Some compatible servers omit ids; the id only needs to survive the result round trip. */
    if (!call->id)
        call->id = xasprintf("call_%d", call->index);

    struct stream_event start = {
        .kind = EV_TOOL_CALL_START,
        .u.tool_call_start = {.id = call->id, .name = call->name},
    };
    emit_event(parser, &start);
    call->started = 1;

    if (call->arguments_before_start.len > 0) {
        struct stream_event arguments = {
            .kind = EV_TOOL_CALL_DELTA,
            .u.tool_call_delta =
                {
                    .id = call->id,
                    .args_delta = call->arguments_before_start.data,
                },
        };
        emit_event(parser, &arguments);
        buf_reset(&call->arguments_before_start);
    }
}

static void handle_text_delta(struct openai_events *parser, const char *text)
{
    if (!text || !*text)
        return;

    struct stream_event event = {
        .kind = EV_TEXT_DELTA,
        .u.text_delta = {.text = text},
    };
    emit_event(parser, &event);
}

static void handle_reasoning_delta(struct openai_events *parser, json_t *delta)
{
    const char *text = json_string_value(json_object_get(delta, "reasoning"));
    if (!text)
        text = json_string_value(json_object_get(delta, "reasoning_content"));
    if (!text || !*text)
        return;

    struct stream_event event = {
        .kind = EV_REASONING_DELTA,
        .u.reasoning_delta = {.text = text},
    };
    emit_event(parser, &event);
}

static void handle_tool_call_delta(struct openai_events *parser, json_t *delta)
{
    /* The specification requires index, but single-call compatible streams often omit it. */
    json_t *index_value = json_object_get(delta, "index");
    int index = json_is_integer(index_value) ? (int)json_integer_value(index_value) : 0;
    struct openai_tool_call *call = get_tool_call(parser, index);

    const char *id = json_string_value(json_object_get(delta, "id"));
    if (id && !call->id)
        call->id = xstrdup(id);

    json_t *function = json_object_get(delta, "function");
    const char *name = json_string_value(json_object_get(function, "name"));
    if (name && !call->name)
        call->name = xstrdup(name);

    start_tool_call(parser, call);

    const char *arguments = json_string_value(json_object_get(function, "arguments"));
    if (!arguments || !*arguments)
        return;
    if (!call->started) {
        buf_append_str(&call->arguments_before_start, arguments);
        return;
    }

    struct stream_event event = {
        .kind = EV_TOOL_CALL_DELTA,
        .u.tool_call_delta = {.id = call->id, .args_delta = arguments},
    };
    emit_event(parser, &event);
}

static void finish_tool_calls(struct openai_events *parser)
{
    for (size_t i = 0; i < parser->n_tool_calls; i++) {
        struct openai_tool_call *call = &parser->tool_calls[i];
        if (!call->started || call->finished)
            continue;

        struct stream_event event = {
            .kind = EV_TOOL_CALL_END,
            .u.tool_call_end = {.id = call->id},
        };
        emit_event(parser, &event);
        call->finished = 1;
    }
}

static void capture_usage(struct openai_events *parser, json_t *root)
{
    json_t *usage = json_object_get(root, "usage");
    if (!json_is_object(usage))
        return;

    json_t *value = json_object_get(usage, "prompt_tokens");
    if (json_is_integer(value))
        parser->usage.input_tokens = (long)json_integer_value(value);

    value = json_object_get(usage, "completion_tokens");
    if (json_is_integer(value))
        parser->usage.output_tokens = (long)json_integer_value(value);

    json_t *details = json_object_get(usage, "prompt_tokens_details");
    if (json_is_object(details)) {
        value = json_object_get(details, "cached_tokens");
        if (json_is_integer(value))
            parser->usage.cached_tokens = (long)json_integer_value(value);

        value = json_object_get(details, "cache_write_tokens");
        if (json_is_integer(value)) {
            parser->usage.cache_write_tokens = (long)json_integer_value(value);
            /* The response does not identify the TTL; only the request does. */
            if (parser->cache_write_1h)
                parser->usage.cache_write_1h_tokens = parser->usage.cache_write_tokens;
        }
    }

    value = json_object_get(usage, "cost");
    if (json_is_number(value) && json_number_value(value) >= 0)
        parser->usage.cost = json_number_value(value);
}

static void handle_progress(struct openai_events *parser, json_t *root)
{
    if (!parser->emit_progress)
        return;

    json_t *progress = json_object_get(root, "prompt_progress");
    if (!json_is_object(progress))
        return;

    struct stream_event event = {
        .kind = EV_PROGRESS,
        .u.progress = {0},
    };
    json_t *value = json_object_get(progress, "processed");
    if (json_is_integer(value))
        event.u.progress.processed = (long)json_integer_value(value);
    value = json_object_get(progress, "total");
    if (json_is_integer(value))
        event.u.progress.total = (long)json_integer_value(value);
    value = json_object_get(progress, "cache");
    if (json_is_integer(value))
        event.u.progress.cache = (long)json_integer_value(value);
    emit_event(parser, &event);
}

static void emit_terminal_event(struct openai_events *parser)
{
    struct stream_response response = response_of(parser);
    if (parser->finish_error) {
        struct stream_event event = {
            .kind = EV_ERROR,
            .u.error =
                {
                    .message = parser->finish_error,
                    .http_status = 0,
                    .usage = &parser->usage,
                    .response = &response,
                },
        };
        emit_event(parser, &event);
        return;
    }

    struct stream_event event = {
        .kind = EV_DONE,
        .u.done =
            {
                .stop_reason = parser->finish_reason ? parser->finish_reason : "stop",
                .usage = parser->usage,
                .response = response,
            },
    };
    emit_event(parser, &event);
}

static void handle_finish_reason(struct openai_events *parser, const char *reason)
{
    if (parser->terminal_emitted || parser->finish_received)
        return;

    finish_tool_calls(parser);
    parser->finish_received = 1;

    int truncated =
        reason && (strcmp(reason, "length") == 0 || strcmp(reason, "content_filter") == 0);
    if (!truncated) {
        parser->finish_reason = xstrdup(reason ? reason : "stop");
        return;
    }

    if (strcmp(reason, "length") == 0 && parser->length_hint)
        parser->finish_error = xasprintf("response incomplete: length — %s", parser->length_hint);
    else
        parser->finish_error = xasprintf("response incomplete: %s", reason);
}

static void handle_done(struct openai_events *parser)
{
    if (parser->terminal_emitted)
        return;

    finish_tool_calls(parser);
    parser->terminal_emitted = 1;
    emit_terminal_event(parser);
}

static void handle_error(struct openai_events *parser, json_t *error)
{
    if (parser->terminal_emitted)
        return;

    parser->terminal_emitted = 1;
    const char *message = json_string_value(json_object_get(error, "message"));
    struct stream_response response = response_of(parser);
    struct stream_event event = {
        .kind = EV_ERROR,
        .u.error =
            {
                .message = message ? message : "provider error",
                .http_status = 0,
                .usage = &parser->usage,
                .response = &response,
            },
    };
    emit_event(parser, &event);
}

static void handle_choice_delta(struct openai_events *parser, json_t *choice)
{
    json_t *delta = json_object_get(choice, "delta");
    if (json_is_object(delta)) {
        handle_reasoning_delta(parser, delta);
        handle_text_delta(parser, json_string_value(json_object_get(delta, "content")));

        json_t *tool_calls = json_object_get(delta, "tool_calls");
        if (json_is_array(tool_calls)) {
            size_t n_tool_calls = json_array_size(tool_calls);
            for (size_t i = 0; i < n_tool_calls; i++)
                handle_tool_call_delta(parser, json_array_get(tool_calls, i));
        }
    }

    const char *finish_reason = json_string_value(json_object_get(choice, "finish_reason"));
    if (finish_reason)
        handle_finish_reason(parser, finish_reason);
}

void openai_events_feed(struct openai_events *parser, const char *data)
{
    if (parser->terminal_emitted || !data || !*data)
        return;
    if (strcmp(data, "[DONE]") == 0) {
        handle_done(parser);
        return;
    }

    json_t *root = json_loads(data, 0, NULL);
    if (!root)
        return;

    json_t *error = json_object_get(root, "error");
    if (json_is_object(error)) {
        handle_error(parser, error);
        json_decref(root);
        return;
    }

    /* Usage and progress chunks may have no choices. */
    capture_response(parser, root);
    capture_usage(parser, root);
    handle_progress(parser, root);

    json_t *choices = json_object_get(root, "choices");
    if (json_is_array(choices) && json_array_size(choices) > 0)
        handle_choice_delta(parser, json_array_get(choices, 0));

    json_decref(root);
}

void openai_events_finalize(struct openai_events *parser)
{
    if (parser->terminal_emitted)
        return;

    parser->terminal_emitted = 1;
    if (parser->finish_received) {
        emit_terminal_event(parser);
        return;
    }

    struct stream_response response = response_of(parser);
    struct stream_event event = {
        .kind = EV_ERROR,
        .u.error =
            {
                .message = "stream ended before completion",
                .http_status = 0,
                .usage = &parser->usage,
                .response = &response,
            },
    };
    emit_event(parser, &event);
}
