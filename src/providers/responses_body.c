/* SPDX-License-Identifier: MIT */
#include "providers/responses_body.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "provider.h"
#include "tool_schema.h"
#include "xalloc.h"
#include "providers/wire.h"

/* Responses accepts content parts instead of a string when a function result contains images. */
static json_t *build_tool_output_parts(const struct item *item, int image_input)
{
    json_t *parts = json_array();
    if (item->output && *item->output)
        json_array_append_new(parts,
                              json_pack("{s:s, s:s}", "type", "input_text", "text", item->output));

    for (size_t i = 0; i < item->n_images; i++) {
        const struct item_image *image = &item->images[i];
        if (image_input != 0) {
            char *url = xasprintf("data:%s;base64,%s", image->mime ? image->mime : "image/png",
                                  image->data_b64 ? image->data_b64 : "");
            json_array_append_new(parts,
                                  json_pack("{s:s, s:s}", "type", "input_image", "image_url", url));
            free(url);
        } else {
            char *placeholder = item_image_placeholder(image);
            json_array_append_new(
                parts, json_pack("{s:s, s:s}", "type", "input_text", "text", placeholder));
            free(placeholder);
        }
    }
    return parts;
}

static json_t *build_message(const char *role, const char *content_type, const char *text)
{
    json_t *content = json_array();
    json_array_append_new(content,
                          json_pack("{s:s, s:s}", "type", content_type, "text", text ? text : ""));
    return json_pack("{s:s, s:s, s:o}", "type", "message", "role", role, "content", content);
}

static json_t *build_reasoning_input(const struct item *item, const char *provider,
                                     const char *model)
{
    /* Encrypted reasoning is bound to its source model. Replaying it after a provider or model
     * switch causes the backend to reject the request. */
    if (!item->reasoning_json || !provider_provenance_matches(item, provider, model))
        return NULL;
    return json_loads(item->reasoning_json, 0, NULL);
}

static json_t *build_input_item(const struct item *item, const char *provider, const char *model,
                                int image_input)
{
    switch (item->kind) {
    case ITEM_USER_MESSAGE:
        return build_message("user", "input_text", item->text);
    case ITEM_ASSISTANT_MESSAGE:
        return build_message("assistant", "output_text", item->text);
    case ITEM_TOOL_CALL:
        return json_pack("{s:s, s:s, s:s, s:s}", "type", "function_call", "call_id",
                         item->call_id ? item->call_id : "", "name",
                         item->tool_name ? item->tool_name : "", "arguments",
                         item->tool_arguments_json ? item->tool_arguments_json : "{}");
    case ITEM_TOOL_RESULT:
        if (item->n_images > 0)
            return json_pack("{s:s, s:s, s:o}", "type", "function_call_output", "call_id",
                             item->call_id ? item->call_id : "", "output",
                             build_tool_output_parts(item, image_input));
        return json_pack("{s:s, s:s, s:s}", "type", "function_call_output", "call_id",
                         item->call_id ? item->call_id : "", "output",
                         item->output ? item->output : "");
    case ITEM_REASONING:
        return build_reasoning_input(item, provider, model);
    case ITEM_TURN_BOUNDARY:
    case ITEM_TURN_USAGE:
        return NULL;
    }
    return NULL;
}

json_t *responses_build_input_items(const struct item *items, size_t n_items, const char *provider,
                                    const char *model, int image_input)
{
    json_t *input = json_array();
    for (size_t i = 0; i < n_items; i++) {
        json_t *input_item = build_input_item(&items[i], provider, model, image_input);
        if (input_item)
            json_array_append_new(input, input_item);
    }
    return input;
}

json_t *responses_build_tools(const struct tool_def *tools, size_t n_tools)
{
    json_t *definitions = json_array();
    for (size_t i = 0; i < n_tools; i++) {
        json_t *parameters = tool_schema_build(&tools[i]);
        json_array_append_new(definitions,
                              json_pack("{s:s, s:s, s:s, s:o}", "type", "function", "name",
                                        tools[i].name, "description", tools[i].description,
                                        "parameters", parameters));
    }
    return definitions;
}

/* An unset effort leaves the level to the backend, which still reasons on a reasoning model, so
 * only an explicit "none" rules reasoning out. Encrypted reasoning is requested whenever reasoning
 * remains possible: with store:false, replaying it is the only way a model carries a chain of
 * thought across the tool calls of one turn. Models that never reason ignore the request. */
static void apply_reasoning(json_t *body, const char *effort)
{
    int disabled = effort && strcmp(effort, "none") == 0;
    if (!disabled) {
        json_t *include = json_array();
        json_array_append_new(include, json_string("reasoning.encrypted_content"));
        json_object_set_new(body, "include", include);
    }

    if (!effort || !*effort)
        return;

    json_t *reasoning = json_object();
    json_object_set_new(reasoning, "effort", json_string(effort));
    if (!disabled)
        json_object_set_new(reasoning, "summary", json_string("auto"));
    json_object_set_new(body, "reasoning", reasoning);
}

json_t *responses_build_body(const struct context *context, const char *provider, const char *model,
                             const struct wire_body_opts *opts)
{
    json_t *input = responses_build_input_items(context->items, context->n_items, provider, model,
                                                context->image_input);
    json_t *body = json_pack("{s:s, s:b, s:b, s:s, s:o}", "model", model, "stream", 1, "store", 0,
                             "instructions", context->system_prompt ? context->system_prompt : "",
                             "input", input);

    if (context->n_tools > 0) {
        json_object_set_new(body, "tools", responses_build_tools(context->tools, context->n_tools));
        json_object_set_new(body, "tool_choice", json_string("auto"));
        json_object_set_new(body, "parallel_tool_calls", json_true());
    }
    apply_reasoning(body, context->effort);
    if (opts && opts->session_cache_key)
        json_object_set_new(body, "prompt_cache_key", json_string(opts->session_cache_key));
    return body;
}
