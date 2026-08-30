/* SPDX-License-Identifier: MIT */
#include "providers/anthropic_body.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "provider.h"
#include "tool_schema.h"
#include "providers/wire.h"

int anthropic_thinking_mode_parse(const char *value)
{
    if (!value)
        return -1;
    if (strcasecmp(value, "adaptive") == 0)
        return ANTHROPIC_THINKING_ADAPTIVE;
    if (strcasecmp(value, "budget") == 0)
        return ANTHROPIC_THINKING_BUDGET;
    if (strcasecmp(value, "off") == 0)
        return ANTHROPIC_THINKING_OFF;
    return -1;
}

int anthropic_effort_expressible(const char *effort)
{
    return strcmp(effort, "none") != 0 && strcmp(effort, "minimal") != 0;
}

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

static json_t *build_cache_control(const char *ttl)
{
    json_t *cache_control = json_pack("{s:s}", "type", "ephemeral");
    if (ttl && strcasecmp(ttl, "1h") == 0)
        json_object_set_new(cache_control, "ttl", json_string("1h"));
    return cache_control;
}

static json_t *build_tools(const struct tool_def *tools, size_t n_tools, int cache_last,
                           const char *ttl)
{
    json_t *tool_list = json_array();
    for (size_t i = 0; i < n_tools; i++) {
        json_t *schema = tool_schema_build(&tools[i]);
        json_t *tool = json_pack("{s:s, s:s, s:o}", "name", tools[i].name, "description",
                                 tools[i].description, "input_schema", schema);
        if (cache_last && i == n_tools - 1)
            json_object_set_new(tool, "cache_control", build_cache_control(ttl));
        json_array_append_new(tool_list, tool);
    }
    return tool_list;
}

static void attach_cache_to_last_message(json_t *messages, const char *ttl)
{
    size_t n_messages = json_array_size(messages);
    if (n_messages == 0)
        return;

    json_t *message = json_array_get(messages, n_messages - 1);
    json_t *content = json_object_get(message, "content");
    if (!json_is_array(content) || json_array_size(content) == 0)
        return;

    json_t *block = json_array_get(content, json_array_size(content) - 1);
    const char *type = json_string_value(json_object_get(block, "type"));
    /* Anthropic rejects cache_control on thinking blocks. */
    if (type && (strcmp(type, "thinking") == 0 || strcmp(type, "redacted_thinking") == 0))
        return;
    json_object_set_new(block, "cache_control", build_cache_control(ttl));
}

static void apply_thinking(json_t *body, const struct context *context,
                           const struct wire_body_opts *opts)
{
    if (opts->thinking_mode == ANTHROPIC_THINKING_OFF)
        return;

    if (opts->thinking_mode == ANTHROPIC_THINKING_ADAPTIVE) {
        const char *display = opts->show_reasoning ? "summarized" : "omitted";
        json_object_set_new(body, "thinking",
                            json_pack("{s:s, s:s}", "type", "adaptive", "display", display));
        if (context->effort && *context->effort) {
            /* A below-floor effort ("minimal") can reach a Messages-routed model on a mixed
             * provider; the wire cannot spell it, so send its Messages minimum instead. */
            const char *effort =
                anthropic_effort_expressible(context->effort) ? context->effort : "low";
            json_object_set_new(body, "output_config", json_pack("{s:s}", "effort", effort));
        }
        return;
    }

    /* Anthropic requires 1 <= budget_tokens < max_tokens. */
    if (opts->max_tokens < 2)
        return;
    int budget_tokens = opts->thinking_budget;
    if (budget_tokens <= 0 || budget_tokens >= opts->max_tokens)
        budget_tokens = opts->max_tokens - 1;
    json_object_set_new(body, "thinking",
                        json_pack("{s:s, s:i}", "type", "enabled", "budget_tokens", budget_tokens));
}

json_t *anthropic_build_body(const struct context *context, const char *provider_id,
                             const char *model, const struct wire_body_opts *opts)
{
    json_t *messages =
        anthropic_build_messages(context->items, context->n_items, provider_id, model,
                                 opts->allow_empty_signature, context->image_input);
    json_t *body = json_pack("{s:s, s:i, s:b, s:o}", "model", model, "max_tokens", opts->max_tokens,
                             "stream", 1, "messages", messages);

    if (context->system_prompt && *context->system_prompt) {
        json_t *system_block =
            json_pack("{s:s, s:s}", "type", "text", "text", context->system_prompt);
        if (opts->cache_markers) {
            json_object_set_new(system_block, "cache_control",
                                build_cache_control(opts->cache_ttl));
        }
        json_t *system = json_array();
        json_array_append_new(system, system_block);
        json_object_set_new(body, "system", system);
    }

    if (context->n_tools > 0) {
        json_object_set_new(
            body, "tools",
            build_tools(context->tools, context->n_tools, opts->cache_markers, opts->cache_ttl));
    }
    if (opts->cache_markers)
        attach_cache_to_last_message(messages, opts->cache_ttl);

    if (opts->temperature)
        json_object_set_new(body, "temperature", json_real(strtod(opts->temperature, NULL)));
    if (opts->top_p)
        json_object_set_new(body, "top_p", json_real(strtod(opts->top_p, NULL)));

    apply_thinking(body, context, opts);
    return body;
}
