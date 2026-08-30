/* SPDX-License-Identifier: MIT */
#include "providers/anthropic_events.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "provider.h"
#include "util.h"

void anthropic_events_init(struct anthropic_events *parser, stream_cb callback, void *callback_user)
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

void anthropic_events_free(struct anthropic_events *parser)
{
    for (size_t i = 0; i < parser->n_blocks; i++) {
        free(parser->blocks[i].tool_call_id);
        free(parser->blocks[i].tool_name);
        free(parser->blocks[i].redacted_data);
        buf_free(&parser->blocks[i].thinking);
        buf_free(&parser->blocks[i].signature);
    }
    free(parser->blocks);
    parser->blocks = NULL;
    parser->n_blocks = parser->block_capacity = 0;
    free(parser->stop_reason);
    parser->stop_reason = NULL;
    free(parser->response_id);
    parser->response_id = NULL;
    free(parser->served_model);
    parser->served_model = NULL;
}

static int emit(struct anthropic_events *parser, const struct stream_event *event)
{
    return parser->callback(event, parser->callback_user);
}

static struct stream_response response_of(const struct anthropic_events *parser)
{
    return (struct stream_response){.id = parser->response_id, .model = parser->served_model};
}

static struct anthropic_content_block *find_block(struct anthropic_events *parser, int index)
{
    for (size_t i = 0; i < parser->n_blocks; i++) {
        if (parser->blocks[i].index == index)
            return &parser->blocks[i];
    }
    return NULL;
}

static struct anthropic_content_block *add_block(struct anthropic_events *parser, int index)
{
    if (parser->n_blocks == parser->block_capacity) {
        size_t capacity = parser->block_capacity ? parser->block_capacity * 2 : 4;
        parser->blocks = xrealloc(parser->blocks, capacity * sizeof(*parser->blocks));
        parser->block_capacity = capacity;
    }

    struct anthropic_content_block *block = &parser->blocks[parser->n_blocks++];
    memset(block, 0, sizeof(*block));
    block->index = index;
    buf_init(&block->thinking);
    buf_init(&block->signature);
    return block;
}

/* An empty delta starts the reasoning indicator when plaintext is omitted. */
static void emit_reasoning_start(struct anthropic_events *parser)
{
    struct stream_event event = {
        .kind = EV_REASONING_DELTA,
        .u.reasoning_delta = {.text = ""},
    };
    emit(parser, &event);
}

static void handle_content_block_start(struct anthropic_events *parser, json_t *root)
{
    json_t *index_value = json_object_get(root, "index");
    int index = json_is_integer(index_value) ? (int)json_integer_value(index_value) : 0;
    json_t *content = json_object_get(root, "content_block");
    const char *type = json_string_value(json_object_get(content, "type"));
    if (!type)
        return;

    struct anthropic_content_block *block = find_block(parser, index);
    if (!block)
        block = add_block(parser, index);

    if (strcmp(type, "text") == 0) {
        block->kind = ANTHROPIC_CONTENT_TEXT;
    } else if (strcmp(type, "thinking") == 0) {
        block->kind = ANTHROPIC_CONTENT_THINKING;
        emit_reasoning_start(parser);
    } else if (strcmp(type, "redacted_thinking") == 0) {
        block->kind = ANTHROPIC_CONTENT_REDACTED_THINKING;
        const char *data = json_string_value(json_object_get(content, "data"));
        if (data)
            block->redacted_data = xstrdup(data);
        emit_reasoning_start(parser);
    } else if (strcmp(type, "tool_use") == 0) {
        block->kind = ANTHROPIC_CONTENT_TOOL_USE;
        const char *id = json_string_value(json_object_get(content, "id"));
        const char *name = json_string_value(json_object_get(content, "name"));
        block->tool_call_id = xstrdup(id ? id : "");
        block->tool_name = xstrdup(name ? name : "");
        struct stream_event event = {
            .kind = EV_TOOL_CALL_START,
            .u.tool_call_start = {.id = block->tool_call_id, .name = block->tool_name},
        };
        emit(parser, &event);
        block->tool_started = 1;
    } else {
        block->kind = ANTHROPIC_CONTENT_OTHER;
    }
}

static void handle_content_block_delta(struct anthropic_events *parser, json_t *root)
{
    json_t *index_value = json_object_get(root, "index");
    int index = json_is_integer(index_value) ? (int)json_integer_value(index_value) : 0;
    json_t *delta = json_object_get(root, "delta");
    const char *type = json_string_value(json_object_get(delta, "type"));
    if (!type)
        return;

    struct anthropic_content_block *block = find_block(parser, index);
    if (strcmp(type, "text_delta") == 0) {
        const char *text = json_string_value(json_object_get(delta, "text"));
        if (text && *text) {
            struct stream_event event = {
                .kind = EV_TEXT_DELTA,
                .u.text_delta = {.text = text},
            };
            emit(parser, &event);
        }
    } else if (strcmp(type, "thinking_delta") == 0) {
        const char *text = json_string_value(json_object_get(delta, "thinking"));
        if (text && *text) {
            if (block)
                buf_append_str(&block->thinking, text);
            struct stream_event event = {
                .kind = EV_REASONING_DELTA,
                .u.reasoning_delta = {.text = text},
            };
            emit(parser, &event);
        }
    } else if (strcmp(type, "signature_delta") == 0) {
        const char *signature = json_string_value(json_object_get(delta, "signature"));
        if (signature && *signature && block)
            buf_append_str(&block->signature, signature);
    } else if (strcmp(type, "input_json_delta") == 0) {
        const char *partial_json = json_string_value(json_object_get(delta, "partial_json"));
        if (partial_json && *partial_json && block && block->tool_started) {
            struct stream_event event = {
                .kind = EV_TOOL_CALL_DELTA,
                .u.tool_call_delta = {.id = block->tool_call_id, .args_delta = partial_json},
            };
            emit(parser, &event);
        }
    }
}

/* Anthropic requires thinking blocks and signatures to be replayed on the next request. */
static void emit_reasoning_item(struct anthropic_events *parser,
                                struct anthropic_content_block *block)
{
    json_t *object = NULL;
    if (block->kind == ANTHROPIC_CONTENT_REDACTED_THINKING) {
        if (!block->redacted_data)
            return;
        object = json_pack("{s:s, s:s}", "type", "redacted_thinking", "data", block->redacted_data);
    } else {
        const char *text = block->thinking.data ? block->thinking.data : "";
        const char *signature = block->signature.data ? block->signature.data : "";
        if (!*text && !*signature)
            return;
        object = json_pack("{s:s, s:s, s:s}", "type", "thinking", "thinking", text, "signature",
                           signature);
    }
    if (!object)
        return;

    char *json = json_dumps(object, JSON_COMPACT);
    json_decref(object);
    if (!json)
        return;

    struct stream_event event = {
        .kind = EV_REASONING_ITEM,
        .u.reasoning_item = {.json = json},
    };
    emit(parser, &event);
    free(json);
}

static void handle_content_block_stop(struct anthropic_events *parser, json_t *root)
{
    json_t *index_value = json_object_get(root, "index");
    int index = json_is_integer(index_value) ? (int)json_integer_value(index_value) : 0;
    struct anthropic_content_block *block = find_block(parser, index);
    if (!block)
        return;

    if (block->kind == ANTHROPIC_CONTENT_TOOL_USE && block->tool_started) {
        struct stream_event event = {
            .kind = EV_TOOL_CALL_END,
            .u.tool_call_end = {.id = block->tool_call_id},
        };
        emit(parser, &event);
    } else if (block->kind == ANTHROPIC_CONTENT_THINKING ||
               block->kind == ANTHROPIC_CONTENT_REDACTED_THINKING) {
        emit_reasoning_item(parser, block);
    }
}

/* Anthropic reports cached input in addition to input_tokens, not as a subset of it. */
static void capture_usage(struct anthropic_events *parser, json_t *usage)
{
    if (!json_is_object(usage))
        return;

    long input_tokens = -1;
    long cache_read_tokens = -1;
    long cache_write_tokens = -1;
    long output_tokens = -1;
    json_t *value;
    if (json_is_integer(value = json_object_get(usage, "input_tokens")))
        input_tokens = (long)json_integer_value(value);
    if (json_is_integer(value = json_object_get(usage, "cache_read_input_tokens")))
        cache_read_tokens = (long)json_integer_value(value);
    if (json_is_integer(value = json_object_get(usage, "cache_creation_input_tokens")))
        cache_write_tokens = (long)json_integer_value(value);
    if (json_is_integer(value = json_object_get(usage, "output_tokens")))
        output_tokens = (long)json_integer_value(value);

    if (input_tokens >= 0) {
        parser->usage.input_tokens = input_tokens;
        if (cache_read_tokens > 0)
            parser->usage.input_tokens += cache_read_tokens;
        if (cache_write_tokens > 0)
            parser->usage.input_tokens += cache_write_tokens;
    }
    if (cache_read_tokens >= 0)
        parser->usage.cached_tokens = cache_read_tokens;
    if (cache_write_tokens >= 0)
        parser->usage.cache_write_tokens = cache_write_tokens;

    json_t *cache_creation = json_object_get(usage, "cache_creation");
    if (json_is_object(cache_creation) &&
        json_is_integer(value = json_object_get(cache_creation, "ephemeral_1h_input_tokens"))) {
        parser->usage.cache_write_1h_tokens = (long)json_integer_value(value);
    }
    if (output_tokens >= 0)
        parser->usage.output_tokens = output_tokens;
}

static void handle_message_start(struct anthropic_events *parser, json_t *root)
{
    json_t *message = json_object_get(root, "message");
    const char *id = json_string_value(json_object_get(message, "id"));
    if (id && *id && !parser->response_id)
        parser->response_id = xstrdup(id);
    const char *model = json_string_value(json_object_get(message, "model"));
    if (model && *model && !parser->served_model)
        parser->served_model = xstrdup(model);
    capture_usage(parser, json_object_get(message, "usage"));
}

static void handle_message_delta(struct anthropic_events *parser, json_t *root)
{
    json_t *delta = json_object_get(root, "delta");
    const char *stop_reason = json_string_value(json_object_get(delta, "stop_reason"));
    if (stop_reason) {
        free(parser->stop_reason);
        parser->stop_reason = xstrdup(stop_reason);
    }
    capture_usage(parser, json_object_get(root, "usage"));
}

static void emit_terminal_error(struct anthropic_events *parser, const char *message)
{
    parser->terminal_emitted = 1;
    struct stream_response response = response_of(parser);
    struct stream_event event = {
        .kind = EV_ERROR,
        .u.error = {.message = message,
                    .http_status = 0,
                    .usage = &parser->usage,
                    .response = &response},
    };
    emit(parser, &event);
}

static void handle_message_stop(struct anthropic_events *parser)
{
    if (parser->terminal_emitted)
        return;

    const char *reason = parser->stop_reason;
    if (reason && strcmp(reason, "max_tokens") == 0) {
        emit_terminal_error(parser, "response incomplete: max_tokens — raise the provider's "
                                    "max_tokens or lower the effort level");
        return;
    }
    /* pause_turn requires replaying server-side tool state, which hax does not drive. */
    if (reason && strcmp(reason, "pause_turn") == 0) {
        emit_terminal_error(parser, "response paused before completion (pause_turn)");
        return;
    }

    parser->terminal_emitted = 1;
    struct stream_event event = {
        .kind = EV_DONE,
        .u.done = {.stop_reason = reason ? reason : "end_turn",
                   .usage = parser->usage,
                   .response = response_of(parser)},
    };
    emit(parser, &event);
}

static void handle_error(struct anthropic_events *parser, json_t *root)
{
    if (parser->terminal_emitted)
        return;

    json_t *error = json_object_get(root, "error");
    const char *message = json_string_value(json_object_get(error, "message"));
    emit_terminal_error(parser, message ? message : "provider error");
}

void anthropic_events_feed(struct anthropic_events *parser, const char *event_name,
                           const char *data)
{
    (void)event_name;
    if (parser->terminal_emitted || !data || !*data)
        return;

    json_t *root = json_loads(data, 0, NULL);
    if (!root)
        return;

    const char *type = json_string_value(json_object_get(root, "type"));
    if (!type)
        goto out;

    if (strcmp(type, "message_start") == 0)
        handle_message_start(parser, root);
    else if (strcmp(type, "content_block_start") == 0)
        handle_content_block_start(parser, root);
    else if (strcmp(type, "content_block_delta") == 0)
        handle_content_block_delta(parser, root);
    else if (strcmp(type, "content_block_stop") == 0)
        handle_content_block_stop(parser, root);
    else if (strcmp(type, "message_delta") == 0)
        handle_message_delta(parser, root);
    else if (strcmp(type, "message_stop") == 0)
        handle_message_stop(parser);
    else if (strcmp(type, "error") == 0)
        handle_error(parser, root);

out:
    json_decref(root);
}

void anthropic_events_finalize(struct anthropic_events *parser)
{
    if (!parser->terminal_emitted)
        emit_terminal_error(parser, "stream ended before completion");
}
