/* SPDX-License-Identifier: MIT */
#include "providers/responses_events.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "provider.h"
#include "xalloc.h"

struct responses_tool_call {
    char *item_id;
    char *call_id;
    int saw_args_delta;
};

static void init_usage(struct stream_usage *usage)
{
    *usage = (struct stream_usage){
        .input_tokens = -1,
        .output_tokens = -1,
        .cached_tokens = -1,
        .cache_write_tokens = -1,
        .cache_write_1h_tokens = -1,
        .cost = -1,
    };
}

void responses_events_init(struct responses_events *events, stream_cb callback, void *callback_user)
{
    memset(events, 0, sizeof(*events));
    events->callback = callback;
    events->callback_user = callback_user;
}

void responses_events_free(struct responses_events *events)
{
    for (size_t i = 0; i < events->tool_call_count; i++) {
        free(events->tool_calls[i].item_id);
        free(events->tool_calls[i].call_id);
    }
    free(events->tool_calls);
    events->tool_calls = NULL;
    events->tool_call_count = 0;
    events->tool_call_capacity = 0;
    free(events->reasoning_item_id);
    events->reasoning_item_id = NULL;
    free(events->response_id);
    events->response_id = NULL;
    free(events->served_model);
    events->served_model = NULL;
}

/* Events consumers close the reasoning block at; see turn_consume and the interactive renderer.
 * Tool argument and end events are invisible there, leaving an open reasoning block open. */
static int event_is_reasoning_seam(enum stream_event_kind kind)
{
    return kind == EV_TEXT_DELTA || kind == EV_TOOL_CALL_START || kind == EV_REASONING_ITEM ||
           kind == EV_DONE || kind == EV_ERROR;
}

static void emit_event(struct responses_events *events, const struct stream_event *event)
{
    /* Part tracking must not survive a seam: a separator injected after one would open the
     * next reasoning block with a stray blank line. */
    if (event_is_reasoning_seam(event->kind)) {
        free(events->reasoning_item_id);
        events->reasoning_item_id = NULL;
    }
    events->callback(event, events->callback_user);
}

static struct responses_tool_call *find_tool_call(struct responses_events *events,
                                                  const char *item_id)
{
    if (!item_id)
        return NULL;

    for (size_t i = 0; i < events->tool_call_count; i++) {
        if (strcmp(events->tool_calls[i].item_id, item_id) == 0)
            return &events->tool_calls[i];
    }
    return NULL;
}

static void add_tool_call(struct responses_events *events, const char *item_id, const char *call_id)
{
    if (events->tool_call_count == events->tool_call_capacity) {
        size_t capacity = events->tool_call_capacity ? events->tool_call_capacity * 2 : 4;
        events->tool_calls = xrealloc(events->tool_calls, capacity * sizeof(*events->tool_calls));
        events->tool_call_capacity = capacity;
    }

    struct responses_tool_call *tool_call = &events->tool_calls[events->tool_call_count++];
    tool_call->item_id = xstrdup(item_id);
    tool_call->call_id = xstrdup(call_id);
    tool_call->saw_args_delta = 0;
}

static void handle_output_item_added(struct responses_events *events, json_t *root)
{
    json_t *item = json_object_get(root, "item");
    const char *type = json_string_value(json_object_get(item, "type"));
    if (!type || strcmp(type, "function_call") != 0)
        return;

    const char *item_id = json_string_value(json_object_get(item, "id"));
    const char *call_id = json_string_value(json_object_get(item, "call_id"));
    const char *name = json_string_value(json_object_get(item, "name"));
    if (!item_id || !call_id || !name)
        return;

    add_tool_call(events, item_id, call_id);
    struct stream_event event = {
        .kind = EV_TOOL_CALL_START,
        .u.tool_call_start = {.id = call_id, .name = name},
    };
    emit_event(events, &event);
}

static void handle_tool_call_done(struct responses_events *events, json_t *item)
{
    const char *item_id = json_string_value(json_object_get(item, "id"));
    struct responses_tool_call *tool_call = find_tool_call(events, item_id);
    if (!tool_call)
        return;

    /* Some backends (OpenCode's Grok, for one) skip argument delta events and deliver the
     * complete arguments only on the item itself. */
    const char *arguments = json_string_value(json_object_get(item, "arguments"));
    if (!tool_call->saw_args_delta && arguments && *arguments) {
        struct stream_event delta_event = {
            .kind = EV_TOOL_CALL_DELTA,
            .u.tool_call_delta = {.id = tool_call->call_id, .args_delta = arguments},
        };
        emit_event(events, &delta_event);
    }

    struct stream_event event = {
        .kind = EV_TOOL_CALL_END,
        .u.tool_call_end = {.id = tool_call->call_id},
    };
    emit_event(events, &event);
}

static void handle_reasoning_item_done(struct responses_events *events, json_t *item)
{
    json_t *encrypted_content = json_object_get(item, "encrypted_content");
    if (!encrypted_content || json_is_null(encrypted_content))
        return;

    /* Output item IDs and unknown output fields are not valid when replaying a reasoning item as
     * Responses API input, so copy only the accepted input fields. */
    json_t *input_item = json_object();
    json_object_set_new(input_item, "type", json_string("reasoning"));
    json_t *summary = json_object_get(item, "summary");
    if (summary)
        json_object_set(input_item, "summary", summary);
    else
        json_object_set_new(input_item, "summary", json_array());
    json_object_set(input_item, "encrypted_content", encrypted_content);

    char *json = json_dumps(input_item, JSON_COMPACT);
    json_decref(input_item);
    if (!json)
        return;

    struct stream_event event = {
        .kind = EV_REASONING_ITEM,
        .u.reasoning_item = {.json = json},
    };
    emit_event(events, &event);
    free(json);
}

static void handle_output_item_done(struct responses_events *events, json_t *root)
{
    json_t *item = json_object_get(root, "item");
    const char *type = json_string_value(json_object_get(item, "type"));
    if (!type)
        return;

    if (strcmp(type, "function_call") == 0)
        handle_tool_call_done(events, item);
    else if (strcmp(type, "reasoning") == 0)
        handle_reasoning_item_done(events, item);
}

static void handle_text_delta(struct responses_events *events, json_t *root)
{
    const char *delta = json_string_value(json_object_get(root, "delta"));
    if (!delta)
        return;

    struct stream_event event = {
        .kind = EV_TEXT_DELTA,
        .u.text_delta = {.text = delta},
    };
    emit_event(events, &event);
}

/* Reasoning summaries and raw reasoning stream as indexed parts with no separator on the wire,
 * so adjacent parts would render glued together. A hard line break puts each part on its own
 * line. Display-only: replay uses the opaque reasoning item, whose summary is copied verbatim,
 * so injected bytes never reach the provider. */
static void emit_reasoning_part_break(struct responses_events *events, json_t *root)
{
    const char *item_id = json_string_value(json_object_get(root, "item_id"));
    json_t *index_value = json_object_get(root, "summary_index");
    int is_content = 0;
    if (!index_value) {
        index_value = json_object_get(root, "content_index");
        is_content = 1;
    }
    if (!item_id || !json_is_integer(index_value))
        return;

    int part_index = (int)json_integer_value(index_value);
    int same_item = events->reasoning_item_id && strcmp(events->reasoning_item_id, item_id) == 0;
    /* A tracked previous part means no EV_REASONING_ITEM sealed it, so an item change needs an
     * injected boundary just like a part change: backends that return no encrypted content give
     * consumers no other seam between consecutive reasoning items. */
    int part_changed =
        events->reasoning_item_id && (!same_item || part_index != events->reasoning_part_index ||
                                      is_content != events->reasoning_part_is_content);
    if (part_changed) {
        struct stream_event event = {
            .kind = EV_REASONING_DELTA,
            .u.reasoning_delta = {.text = "  \n"},
        };
        emit_event(events, &event);
    }
    if (!same_item) {
        free(events->reasoning_item_id);
        events->reasoning_item_id = xstrdup(item_id);
    }
    events->reasoning_part_index = part_index;
    events->reasoning_part_is_content = is_content;
}

static void handle_reasoning_delta(struct responses_events *events, json_t *root)
{
    const char *delta = json_string_value(json_object_get(root, "delta"));
    if (!delta || !*delta)
        return;

    emit_reasoning_part_break(events, root);
    struct stream_event event = {
        .kind = EV_REASONING_DELTA,
        .u.reasoning_delta = {.text = delta},
    };
    emit_event(events, &event);
}

static void handle_tool_call_delta(struct responses_events *events, json_t *root)
{
    const char *item_id = json_string_value(json_object_get(root, "item_id"));
    const char *delta = json_string_value(json_object_get(root, "delta"));
    /* An empty delta carries nothing and must not count as streamed arguments, or it would
     * defeat the completed-item fallback in handle_tool_call_done. */
    if (!item_id || !delta || !*delta)
        return;

    struct responses_tool_call *tool_call = find_tool_call(events, item_id);
    if (!tool_call)
        return;

    tool_call->saw_args_delta = 1;
    struct stream_event event = {
        .kind = EV_TOOL_CALL_DELTA,
        .u.tool_call_delta = {.id = tool_call->call_id, .args_delta = delta},
    };
    emit_event(events, &event);
}

static void capture_response(struct responses_events *events, json_t *root)
{
    json_t *response = root ? json_object_get(root, "response") : NULL;
    if (!json_is_object(response))
        return;

    const char *id = json_string_value(json_object_get(response, "id"));
    if (id && *id && !events->response_id)
        events->response_id = xstrdup(id);
    const char *model = json_string_value(json_object_get(response, "model"));
    if (model && *model && !events->served_model)
        events->served_model = xstrdup(model);
}

static struct stream_response response_of(const struct responses_events *events)
{
    return (struct stream_response){.id = events->response_id, .model = events->served_model};
}

/* Usage arrives on terminal events under response.usage. A missing cached-token field is
 * unknown rather than a known cache miss. */
static void parse_usage(json_t *root, struct stream_usage *usage)
{
    init_usage(usage);

    json_t *response = root ? json_object_get(root, "response") : NULL;
    json_t *response_usage = json_object_get(response, "usage");
    if (!json_is_object(response_usage))
        return;

    json_t *value = json_object_get(response_usage, "input_tokens");
    if (json_is_integer(value))
        usage->input_tokens = (long)json_integer_value(value);

    value = json_object_get(response_usage, "output_tokens");
    if (json_is_integer(value))
        usage->output_tokens = (long)json_integer_value(value);

    json_t *details = json_object_get(response_usage, "input_tokens_details");
    value = json_object_get(details, "cached_tokens");
    if (json_is_integer(value))
        usage->cached_tokens = (long)json_integer_value(value);
}

static void emit_terminal_error(struct responses_events *events, const char *message, json_t *root)
{
    if (events->terminal_emitted)
        return;

    events->terminal_emitted = 1;
    struct stream_usage usage;
    parse_usage(root, &usage);
    struct stream_response response = response_of(events);
    struct stream_event event = {
        .kind = EV_ERROR,
        .u.error = {.message = message, .http_status = 0, .usage = &usage, .response = &response},
    };
    emit_event(events, &event);
}

static void handle_failed(struct responses_events *events, json_t *root)
{
    json_t *response = json_object_get(root, "response");
    json_t *error = json_object_get(response, "error");
    const char *message = json_string_value(json_object_get(error, "message"));
    emit_terminal_error(events, message ? message : "response.failed", root);
}

/* A bare `error` event carries the failure at the top level rather than under `response`, and no
 * terminal response event follows it. */
static void handle_stream_error(struct responses_events *events, json_t *root)
{
    const char *message = json_string_value(json_object_get(root, "message"));
    const char *code = json_string_value(json_object_get(root, "code"));
    if (!message)
        message = code;
    emit_terminal_error(events, message ? message : "provider error", root);
}

static void handle_incomplete(struct responses_events *events, json_t *root)
{
    json_t *response = json_object_get(root, "response");
    json_t *details = json_object_get(response, "incomplete_details");
    const char *reason = json_string_value(json_object_get(details, "reason"));
    char *message = xasprintf("response incomplete: %s", reason ? reason : "unknown");
    emit_terminal_error(events, message, root);
    free(message);
}

static void handle_completed(struct responses_events *events, json_t *root)
{
    if (events->terminal_emitted)
        return;

    events->terminal_emitted = 1;
    struct stream_event event = {
        .kind = EV_DONE,
        .u.done = {.stop_reason = "completed", .response = response_of(events)},
    };
    parse_usage(root, &event.u.done.usage);
    emit_event(events, &event);
}

void responses_events_feed(struct responses_events *events, const char *data)
{
    if (!data || !*data)
        return;
    if (strcmp(data, "[DONE]") == 0) {
        handle_completed(events, NULL);
        return;
    }

    json_t *root = json_loads(data, 0, NULL);
    if (!root)
        return;

    const char *type = json_string_value(json_object_get(root, "type"));
    if (!type)
        goto out;

    capture_response(events, root);

    if (strcmp(type, "response.output_item.added") == 0)
        handle_output_item_added(events, root);
    else if (strcmp(type, "response.output_item.done") == 0)
        handle_output_item_done(events, root);
    /* A refusal is the assistant's answer, carried in its own content part. Dropping it would
     * complete the response with no text at all. */
    else if (strcmp(type, "response.output_text.delta") == 0 ||
             strcmp(type, "response.refusal.delta") == 0)
        handle_text_delta(events, root);
    else if (strcmp(type, "response.reasoning_summary_text.delta") == 0 ||
             strcmp(type, "response.reasoning_text.delta") == 0)
        handle_reasoning_delta(events, root);
    else if (strcmp(type, "response.function_call_arguments.delta") == 0)
        handle_tool_call_delta(events, root);
    else if (strcmp(type, "response.completed") == 0 || strcmp(type, "response.done") == 0)
        handle_completed(events, root);
    else if (strcmp(type, "response.incomplete") == 0)
        handle_incomplete(events, root);
    else if (strcmp(type, "response.failed") == 0)
        handle_failed(events, root);
    else if (strcmp(type, "error") == 0)
        handle_stream_error(events, root);

out:
    json_decref(root);
}

void responses_events_finalize(struct responses_events *events)
{
    emit_terminal_error(events, "stream ended before completion", NULL);
}
