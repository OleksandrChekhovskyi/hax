/* SPDX-License-Identifier: MIT */
#include "providers/chat_body.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "buf.h"
#include "catalog.h"
#include "diag.h"
#include "provider.h"
#include "tool_schema.h"
#include "util.h"
#include "providers/wire.h"

/* AUTO sends explicit cache markers only when writes replace ordinary input processing. */
struct chat_cache_plan chat_plan_cache(const struct catalog_entry *rates, enum chat_cache_mode mode,
                                       const char *ttl)
{
    struct chat_cache_plan plan = {0};
    plan.send_breakpoints = mode == CHAT_CACHE_ON ||
                            (mode == CHAT_CACHE_AUTO && catalog_cache_write_replaces_input(rates));
    plan.writes_bill_1h = plan.send_breakpoints && ttl && strcasecmp(ttl, "1h") == 0 &&
                          rates->cost_cache_write_1h >= 0;
    return plan;
}

static json_t *build_tool_call(const struct item *item)
{
    return json_pack("{s:s, s:s, s:{s:s, s:s}}", "id", item->call_id ? item->call_id : "", "type",
                     "function", "function", "name", item->tool_name ? item->tool_name : "",
                     "arguments", item->tool_arguments_json ? item->tool_arguments_json : "{}");
}

/* Typed reasoning blocks must reach the model unchanged and in their original order, so the
 * items of one assistant message concatenate rather than replace. A blob of any other shape
 * belongs to another wire's encoding and is left alone. */
static void collect_reasoning_details(json_t **details, const char *reasoning_json)
{
    if (!reasoning_json)
        return;

    json_t *parsed = json_loads(reasoning_json, 0, NULL);
    if (!json_is_array(parsed)) {
        json_decref(parsed);
        return;
    }

    if (!*details)
        *details = json_array();
    size_t index;
    json_t *detail;
    json_array_foreach(parsed, index, detail) json_array_append(*details, detail);
    json_decref(parsed);
}

/* Chat Completions cannot preserve text/tool-call interleaving within an assistant message. */
static size_t append_assistant_message(json_t *messages, const struct item *items, size_t index,
                                       size_t n_items, const char *reasoning_field,
                                       const char *current_provider, const char *current_model)
{
    struct buf text;
    struct buf reasoning;
    buf_init(&text);
    buf_init(&reasoning);
    json_t *tool_calls = NULL;
    json_t *details = NULL;

    while (index < n_items &&
           (items[index].kind == ITEM_ASSISTANT_MESSAGE || items[index].kind == ITEM_TOOL_CALL ||
            items[index].kind == ITEM_REASONING)) {
        const struct item *item = &items[index++];
        switch (item->kind) {
        case ITEM_ASSISTANT_MESSAGE:
            if (item->text)
                buf_append_str(&text, item->text);
            break;
        case ITEM_REASONING:
            if (!provider_provenance_matches(item, current_provider, current_model))
                break;
            if (item->reasoning_text && *item->reasoning_text) {
                if (reasoning.len > 0)
                    buf_append_str(&reasoning, "\n");
                buf_append_str(&reasoning, item->reasoning_text);
            }
            collect_reasoning_details(&details, item->reasoning_json);
            break;
        case ITEM_TOOL_CALL:
            if (!tool_calls)
                tool_calls = json_array();
            json_array_append_new(tool_calls, build_tool_call(item));
            break;
        default:
            break;
        }
    }

    /* The typed sequence is the richer encoding of the same reasoning: sending the plain member
     * alongside it would duplicate the content. */
    int include_reasoning = reasoning_field && reasoning.len > 0 && !details;
    if (text.len == 0 && !tool_calls && !include_reasoning && !details)
        goto out;

    json_t *message = json_object();
    json_object_set_new(message, "role", json_string("assistant"));
    json_object_set_new(message, "content", text.len > 0 ? json_string(text.data) : json_null());
    if (tool_calls)
        json_object_set_new(message, "tool_calls", tool_calls);
    if (details)
        json_object_set_new(message, "reasoning_details", details);
    else if (include_reasoning)
        json_object_set_new(message, reasoning_field, json_string(reasoning.data));
    json_array_append_new(messages, message);

out:
    buf_free(&reasoning);
    buf_free(&text);
    return index;
}

static char *tool_result_text(const struct item *item)
{
    struct buf text;
    buf_init(&text);
    if (item->output)
        buf_append_str(&text, item->output);

    for (size_t i = 0; i < item->n_images; i++) {
        char *placeholder = item_image_placeholder(&item->images[i]);
        if (text.len > 0)
            buf_append_str(&text, "\n");
        buf_append_str(&text, placeholder);
        free(placeholder);
    }
    return text.data;
}

static void append_tool_result(json_t *messages, const struct item *item, int image_input)
{
    char *text_with_placeholders = NULL;
    const char *content = item->output ? item->output : "";
    if (item->n_images > 0 && image_input == 0) {
        text_with_placeholders = tool_result_text(item);
        content = text_with_placeholders ? text_with_placeholders : "";
    }

    json_array_append_new(messages,
                          json_pack("{s:s, s:s, s:s}", "role", "tool", "tool_call_id",
                                    item->call_id ? item->call_id : "", "content", content));
    free(text_with_placeholders);
}

static void append_tool_result_images(json_t *messages, const struct item *items, size_t first,
                                      size_t end)
{
    json_t *parts = NULL;
    for (size_t i = first; i < end; i++) {
        for (size_t j = 0; j < items[i].n_images; j++) {
            const struct item_image *image = &items[i].images[j];
            if (!parts) {
                parts = json_array();
                json_array_append_new(parts,
                                      json_pack("{s:s, s:s}", "type", "text", "text",
                                                "Image(s) from the preceding tool result(s):"));
            }

            char *url = xasprintf("data:%s;base64,%s", image->mime ? image->mime : "image/png",
                                  image->data_b64 ? image->data_b64 : "");
            json_array_append_new(
                parts, json_pack("{s:s, s:{s:s}}", "type", "image_url", "image_url", "url", url));
            free(url);
        }
    }

    if (parts)
        json_array_append_new(messages, json_pack("{s:s, s:o}", "role", "user", "content", parts));
}

static size_t append_tool_results(json_t *messages, const struct item *items, size_t index,
                                  size_t n_items, int image_input)
{
    size_t first = index;
    while (index < n_items && items[index].kind == ITEM_TOOL_RESULT) {
        append_tool_result(messages, &items[index], image_input);
        index++;
    }

    /* Tool-role content must remain strings, and parallel results must remain adjacent. */
    if (image_input != 0)
        append_tool_result_images(messages, items, first, index);
    return index;
}

json_t *chat_build_messages(const char *system_prompt, const struct item *items, size_t n_items,
                            const char *reasoning_field, const char *current_provider,
                            const char *current_model, int image_input)
{
    json_t *messages = json_array();
    if (system_prompt && *system_prompt) {
        json_array_append_new(messages,
                              json_pack("{s:s, s:s}", "role", "system", "content", system_prompt));
    }

    size_t index = 0;
    while (index < n_items) {
        switch (items[index].kind) {
        case ITEM_USER_MESSAGE:
            json_array_append_new(messages, json_pack("{s:s, s:s}", "role", "user", "content",
                                                      items[index].text ? items[index].text : ""));
            index++;
            break;
        case ITEM_ASSISTANT_MESSAGE:
        case ITEM_TOOL_CALL:
            index = append_assistant_message(messages, items, index, n_items, reasoning_field,
                                             current_provider, current_model);
            break;
        case ITEM_TOOL_RESULT:
            index = append_tool_results(messages, items, index, n_items, image_input);
            break;
        case ITEM_REASONING:
            if (items[index].reasoning_text || items[index].reasoning_json) {
                index = append_assistant_message(messages, items, index, n_items, reasoning_field,
                                                 current_provider, current_model);
            } else {
                index++;
            }
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

static int attach_cache_control(json_t *message, const char *ttl)
{
    json_t *content = json_object_get(message, "content");
    if (json_is_string(content)) {
        json_t *part = json_pack("{s:s, s:O}", "type", "text", "text", content);
        json_object_set_new(part, "cache_control", build_cache_control(ttl));
        json_t *parts = json_array();
        json_array_append_new(parts, part);
        json_object_set_new(message, "content", parts);
        return 1;
    }
    if (!json_is_array(content) || json_array_size(content) == 0)
        return 0;

    json_t *last = json_array_get(content, json_array_size(content) - 1);
    json_object_set_new(last, "cache_control", build_cache_control(ttl));
    return 1;
}

void chat_apply_cache_breakpoints(json_t *messages, const char *ttl)
{
    size_t n_messages = json_array_size(messages);
    if (n_messages == 0)
        return;

    size_t tail_floor = 0;
    json_t *first = json_array_get(messages, 0);
    const char *role = json_string_value(json_object_get(first, "role"));
    if (role && strcmp(role, "system") == 0 && attach_cache_control(first, ttl))
        tail_floor = 1;

    /* A contentless tool-call message cannot carry the tail breakpoint. */
    for (size_t i = n_messages; i-- > tail_floor;) {
        if (attach_cache_control(json_array_get(messages, i), ttl))
            return;
    }
}

enum chat_reasoning_format chat_reasoning_format_parse(const char *value,
                                                       enum chat_reasoning_format fallback)
{
    if (!value || !*value)
        return fallback;
    if (strcasecmp(value, "flat") == 0)
        return CHAT_REASONING_FLAT;
    if (strcasecmp(value, "nested") == 0)
        return CHAT_REASONING_NESTED;

    hax_warn("unknown reasoning format %s (expected 'flat' or 'nested') — using default", value);
    return fallback;
}

void chat_apply_reasoning(json_t *body, enum chat_reasoning_format format, const char *effort)
{
    if (!effort || !*effort)
        return;

    switch (format) {
    case CHAT_REASONING_FLAT:
        json_object_set_new(body, "reasoning_effort", json_string(effort));
        break;
    case CHAT_REASONING_NESTED: {
        int enabled = strcmp(effort, "none") != 0;
        json_t *reasoning = json_pack("{s:b}", "enabled", enabled);
        if (enabled)
            json_object_set_new(reasoning, "effort", json_string(effort));
        json_object_set_new(body, "reasoning", reasoning);
        break;
    }
    }
}

static json_t *build_tools(const struct tool_def *tools, size_t n_tools)
{
    json_t *tool_list = json_array();
    for (size_t i = 0; i < n_tools; i++) {
        json_t *parameters = tool_schema_build(&tools[i]);
        json_array_append_new(tool_list, json_pack("{s:s, s:{s:s, s:s, s:o}}", "type", "function",
                                                   "function", "name", tools[i].name, "description",
                                                   tools[i].description, "parameters", parameters));
    }
    return tool_list;
}

json_t *chat_build_body(const struct context *context, const char *provider_id, const char *model,
                        const struct wire_body_opts *opts)
{
    json_t *messages =
        chat_build_messages(context->system_prompt, context->items, context->n_items,
                            opts->reasoning_field, provider_id, model, context->image_input);
    if (opts->cache_markers)
        chat_apply_cache_breakpoints(messages, opts->cache_ttl);

    /* Usage is requested on every stream so terminal events can report token counts. */
    json_t *body = json_pack("{s:s, s:b, s:o, s:{s:b}}", "model", model, "stream", 1, "messages",
                             messages, "stream_options", "include_usage", 1);

    if (context->n_tools > 0)
        json_object_set_new(body, "tools", build_tools(context->tools, context->n_tools));
    if (opts->session_cache_key)
        json_object_set_new(body, "prompt_cache_key", json_string(opts->session_cache_key));
    if (opts->request_cost)
        json_object_set_new(body, "usage", json_pack("{s:b}", "include", 1));

    chat_apply_reasoning(body, opts->reasoning_format, context->effort);
    return body;
}
