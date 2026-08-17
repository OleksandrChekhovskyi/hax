/* SPDX-License-Identifier: MIT */
#include "providers/anthropic_messages.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "provider.h"

static json_t *build_text_block(const char *text)
{
    return json_pack("{s:s, s:s}", "type", "text", "text", text ? text : "");
}

static void append_reasoning_block(json_t *content, const struct item *item,
                                   int allow_empty_signature)
{
    if (!item->reasoning_json)
        return;

    json_t *block = json_loads(item->reasoning_json, 0, NULL);
    if (!block)
        return;

    const char *type = json_string_value(json_object_get(block, "type"));
    if (type && strcmp(type, "thinking") == 0 && !allow_empty_signature) {
        const char *signature = json_string_value(json_object_get(block, "signature"));
        if (!signature || !*signature) {
            const char *thinking = json_string_value(json_object_get(block, "thinking"));
            if (thinking && *thinking)
                json_array_append_new(content, build_text_block(thinking));
            json_decref(block);
            return;
        }
    }
    json_array_append_new(content, block);
}

static json_t *build_tool_use_block(const struct item *item)
{
    json_t *input =
        item->tool_arguments_json ? json_loads(item->tool_arguments_json, 0, NULL) : NULL;
    if (!json_is_object(input)) {
        json_decref(input);
        input = json_object();
    }

    return json_pack("{s:s, s:s, s:s, s:o}", "type", "tool_use", "id",
                     item->call_id ? item->call_id : "", "name",
                     item->tool_name ? item->tool_name : "", "input", input);
}

static size_t append_assistant_message(json_t *messages, const struct item *items, size_t index,
                                       size_t n_items, const char *current_provider,
                                       const char *current_model, int allow_empty_signature)
{
    json_t *content = json_array();
    while (index < n_items &&
           (items[index].kind == ITEM_ASSISTANT_MESSAGE || items[index].kind == ITEM_TOOL_CALL ||
            items[index].kind == ITEM_REASONING)) {
        const struct item *item = &items[index++];
        switch (item->kind) {
        case ITEM_ASSISTANT_MESSAGE:
            if (item->text && *item->text)
                json_array_append_new(content, build_text_block(item->text));
            break;
        case ITEM_REASONING:
            if (provider_provenance_matches(item, current_provider, current_model))
                append_reasoning_block(content, item, allow_empty_signature);
            break;
        case ITEM_TOOL_CALL:
            json_array_append_new(content, build_tool_use_block(item));
            break;
        default:
            break;
        }
    }

    if (json_array_size(content) == 0) {
        json_decref(content);
    } else {
        json_array_append_new(messages,
                              json_pack("{s:s, s:o}", "role", "assistant", "content", content));
    }
    return index;
}

static json_t *build_image_block(const struct item_image *image)
{
    return json_pack("{s:s, s:{s:s, s:s, s:s}}", "type", "image", "source", "type", "base64",
                     "media_type", image->mime ? image->mime : "image/png", "data",
                     image->data_b64 ? image->data_b64 : "");
}

static json_t *build_tool_result_block(const struct item *item, int image_input)
{
    if (item->n_images == 0) {
        return json_pack("{s:s, s:s, s:s}", "type", "tool_result", "tool_use_id",
                         item->call_id ? item->call_id : "", "content",
                         item->output ? item->output : "");
    }

    json_t *content = json_array();
    if (item->output && *item->output)
        json_array_append_new(content, build_text_block(item->output));
    for (size_t i = 0; i < item->n_images; i++) {
        if (image_input != 0) {
            json_array_append_new(content, build_image_block(&item->images[i]));
        } else {
            char *placeholder = item_image_placeholder(&item->images[i]);
            json_array_append_new(content, build_text_block(placeholder));
            free(placeholder);
        }
    }

    return json_pack("{s:s, s:s, s:o}", "type", "tool_result", "tool_use_id",
                     item->call_id ? item->call_id : "", "content", content);
}

static size_t append_tool_results(json_t *messages, const struct item *items, size_t index,
                                  size_t n_items, int image_input)
{
    json_t *content = json_array();
    while (index < n_items && items[index].kind == ITEM_TOOL_RESULT)
        json_array_append_new(content, build_tool_result_block(&items[index++], image_input));

    json_array_append_new(messages, json_pack("{s:s, s:o}", "role", "user", "content", content));
    return index;
}

json_t *anthropic_build_messages(const struct item *items, size_t n_items,
                                 const char *current_provider, const char *current_model,
                                 int allow_empty_signature, int image_input)
{
    json_t *messages = json_array();
    size_t index = 0;
    while (index < n_items) {
        switch (items[index].kind) {
        case ITEM_USER_MESSAGE: {
            json_t *content = json_array();
            json_array_append_new(content, build_text_block(items[index].text));
            json_array_append_new(messages,
                                  json_pack("{s:s, s:o}", "role", "user", "content", content));
            index++;
            break;
        }
        case ITEM_ASSISTANT_MESSAGE:
        case ITEM_TOOL_CALL:
        case ITEM_REASONING:
            index = append_assistant_message(messages, items, index, n_items, current_provider,
                                             current_model, allow_empty_signature);
            break;
        case ITEM_TOOL_RESULT:
            index = append_tool_results(messages, items, index, n_items, image_input);
            break;
        case ITEM_TURN_BOUNDARY:
        case ITEM_TURN_USAGE:
            index++;
            break;
        }
    }
    return messages;
}
