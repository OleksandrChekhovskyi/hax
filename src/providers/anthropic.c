/* SPDX-License-Identifier: MIT */
#include "providers/anthropic.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "effort.h"
#include "model_meta.h"
#include "provider.h"
#include "tool_schema.h"
#include "util.h"
#include "providers/anthropic_events.h"
#include "providers/anthropic_messages.h"
#include "providers/config_provider.h"
#include "transport/api_error.h"
#include "transport/http.h"
#include "transport/retry.h"

#define ANTHROPIC_DEFAULT_VERSION    "2023-06-01"
#define ANTHROPIC_DEFAULT_MAX_TOKENS 32000

/* Bound foreground pagination if a server keeps returning advancing cursors. */
#define ANTHROPIC_MODEL_PAGE_SIZE  1000
#define ANTHROPIC_MODEL_PAGE_LIMIT 50

#define MODEL_LIST_TIMEOUT_S  10
#define MODEL_PROBE_TIMEOUT_S 5

const char *const ANTHROPIC_EFFORT_LADDER[] = {"low", "medium", "high", "xhigh", "max"};
const size_t ANTHROPIC_EFFORT_LADDER_N =
    sizeof(ANTHROPIC_EFFORT_LADDER) / sizeof(ANTHROPIC_EFFORT_LADDER[0]);

struct anthropic {
    struct provider base;
    char *base_url;
    char *api_key;
    char *name;
    char *catalog_id;
    char *endpoint;
    char *version;
    char *config_prefix;
    char **extra_headers;
    json_t *extra_body;
    const char *cache_ttl; /* canonical static "5m"/"1h" from provider_cache_ttl */
    enum anthropic_thinking_mode default_thinking_mode;
    int allow_empty_signature;
    int cache_default;
};

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

static enum anthropic_thinking_mode resolve_thinking_mode(const struct anthropic *anthropic)
{
    const char *configured = config_scoped_str(anthropic->config_prefix, "thinking_mode");
    if (!configured || !*configured)
        return anthropic->default_thinking_mode;
    if (strcasecmp(configured, "adaptive") == 0)
        return ANTHROPIC_THINKING_ADAPTIVE;
    if (strcasecmp(configured, "budget") == 0)
        return ANTHROPIC_THINKING_BUDGET;
    if (strcasecmp(configured, "off") == 0)
        return ANTHROPIC_THINKING_OFF;

    hax_warn("unknown thinking_mode '%s' (adaptive/budget/off) — using default", configured);
    return anthropic->default_thinking_mode;
}

static void apply_thinking(json_t *body, const struct anthropic *anthropic,
                           const struct context *context, int max_tokens)
{
    enum anthropic_thinking_mode mode = resolve_thinking_mode(anthropic);
    if (mode == ANTHROPIC_THINKING_OFF)
        return;

    if (mode == ANTHROPIC_THINKING_ADAPTIVE) {
        const char *display = config_bool("show_reasoning") ? "summarized" : "omitted";
        json_object_set_new(body, "thinking",
                            json_pack("{s:s, s:s}", "type", "adaptive", "display", display));
        if (context->effort && *context->effort) {
            json_object_set_new(body, "output_config",
                                json_pack("{s:s}", "effort", context->effort));
        }
        return;
    }

    /* Anthropic requires 1 <= budget_tokens < max_tokens. */
    if (max_tokens < 2)
        return;
    int budget_tokens = config_scoped_int(anthropic->config_prefix, "thinking_budget");
    if (budget_tokens <= 0 || budget_tokens >= max_tokens)
        budget_tokens = max_tokens - 1;
    json_object_set_new(body, "thinking",
                        json_pack("{s:s, s:i}", "type", "enabled", "budget_tokens", budget_tokens));
}

int anthropic_max_tokens(struct provider *provider, const char *model)
{
    struct anthropic *anthropic = (struct anthropic *)provider;
    int configured = 0;
    int user_set = 0;
    if (anthropic->config_prefix) {
        char *key = xasprintf("%s.max_tokens", anthropic->config_prefix);
        configured = config_int(key);
        user_set = strcmp(config_source(key), "default") != 0;
        free(key);
    }

    long model_limit = model_meta_max_output(provider, model);
    if (user_set && configured > 0)
        return model_limit > 0 && configured > model_limit ? (int)model_limit : configured;
    if (model_limit > 0)
        return (int)model_limit;
    return configured > 0 ? configured : ANTHROPIC_DEFAULT_MAX_TOKENS;
}

static char *build_request_body(struct anthropic *anthropic, const struct context *context,
                                const char *model)
{
    int max_tokens = anthropic_max_tokens(&anthropic->base, model);
    int cache = config_scoped_bool_or(anthropic->config_prefix, "cache", anthropic->cache_default);
    const char *cache_ttl = anthropic->cache_ttl;

    json_t *messages = anthropic_build_messages(
        context->items, context->n_items, provider_stable_id(&anthropic->base), model,
        anthropic->allow_empty_signature, context->image_input);
    json_t *body = json_pack("{s:s, s:i, s:b, s:o}", "model", model, "max_tokens", max_tokens,
                             "stream", 1, "messages", messages);

    if (context->system_prompt && *context->system_prompt) {
        json_t *system_block =
            json_pack("{s:s, s:s}", "type", "text", "text", context->system_prompt);
        if (cache)
            json_object_set_new(system_block, "cache_control", build_cache_control(cache_ttl));
        json_t *system = json_array();
        json_array_append_new(system, system_block);
        json_object_set_new(body, "system", system);
    }

    if (context->n_tools > 0) {
        json_object_set_new(body, "tools",
                            build_tools(context->tools, context->n_tools, cache, cache_ttl));
    }
    if (cache)
        attach_cache_to_last_message(messages, cache_ttl);

    apply_thinking(body, anthropic, context, max_tokens);
    provider_extra_body_apply(body, anthropic->extra_body);

    char *json = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    return json;
}

/* Owned NULL-terminated request headers; free with string_array_free. Streaming requests add
 * the SSE Accept and the JSON Content-Type. */
static char **build_request_headers(const struct anthropic *anthropic, int streaming)
{
    char *api_key_header =
        anthropic->api_key ? xasprintf("x-api-key: %s", anthropic->api_key) : NULL;
    char *version_header = xasprintf("anthropic-version: %s", anthropic->version);
    const char *fixed[5];
    size_t n_fixed = 0;
    if (api_key_header)
        fixed[n_fixed++] = api_key_header;
    fixed[n_fixed++] = version_header;
    if (streaming) {
        fixed[n_fixed++] = "Accept: text/event-stream";
        fixed[n_fixed++] = "Content-Type: application/json";
    }
    fixed[n_fixed] = NULL;

    char **headers = string_array_concat(fixed, (const char *const *)anthropic->extra_headers);
    free(api_key_header);
    free(version_header);
    return headers;
}

static int handle_sse_data(const char *event_name, const char *data, void *user)
{
    anthropic_events_feed(user, event_name, data);
    return 0;
}

static int anthropic_stream(struct provider *provider, const struct context *context,
                            const char *model, stream_cb callback, void *callback_user,
                            http_tick_cb tick, void *tick_user)
{
    struct anthropic *anthropic = (struct anthropic *)provider;

    char *body = build_request_body(anthropic, context, model);
    if (!body)
        return -1;

    size_t body_len = strlen(body);
    char **request_headers = build_request_headers(anthropic, 1);

    struct retry_policy policy = retry_policy_default();
    struct http_response response;
    struct anthropic_events parser;
    int result = -1;

    /* Each retry needs fresh parser state; request bytes are safe to resend unchanged. */
    for (int attempt = 0; attempt < policy.max_attempts; attempt++) {
        memset(&response, 0, sizeof(response));
        anthropic_events_init(&parser, callback, callback_user);
        result = http_sse_post(anthropic->endpoint, (const char *const *)request_headers, body,
                               body_len, policy.idle_timeout_s, handle_sse_data, &parser, tick,
                               tick_user, &response);

        if (response.cancelled ||
            !retry_should_attempt(result, response.status, response.error_body) ||
            attempt + 1 >= policy.max_attempts) {
            break;
        }

        long delay_ms = response.retry_after_ms > 0 ? response.retry_after_ms
                                                    : retry_delay_ms(&policy, attempt);
        struct stream_event retry = {
            .kind = EV_RETRY,
            .u.retry =
                {
                    .attempt = attempt + 1,
                    .max_attempts = policy.max_attempts,
                    .delay_ms = delay_ms,
                    .http_status = (int)response.status,
                },
        };
        callback(&retry, callback_user);

        free(response.error_body);
        response.error_body = NULL;
        anthropic_events_free(&parser);

        if (retry_sleep_with_tick(delay_ms, tick, tick_user)) {
            response.cancelled = 1;
            memset(&parser, 0, sizeof(parser));
            break;
        }
    }

    if (!response.cancelled) {
        if (result != 0 || response.status < 200 || response.status >= 300) {
            char *message = format_api_error(response.status, response.error_body);
            struct stream_event error = {
                .kind = EV_ERROR,
                .u.error = {.message = message, .http_status = (int)response.status},
            };
            callback(&error, callback_user);
            free(message);
        } else {
            anthropic_events_finalize(&parser);
        }
    }

    free(response.error_body);
    anthropic_events_free(&parser);
    string_array_free(request_headers);
    free(body);
    return result;
}

static void parse_model_efforts(json_t *effort, struct effort_set *out)
{
    if (json_is_false(json_object_get(effort, "supported"))) {
        out->known = 1;
        return;
    }
    if (!json_is_object(effort))
        return;

    /* Preserve the picker order rather than the JSON object's member order. */
    for (size_t i = 0; i < ANTHROPIC_EFFORT_LADDER_N; i++) {
        json_t *level = json_object_get(effort, ANTHROPIC_EFFORT_LADDER[i]);
        if (json_is_object(level) && json_is_true(json_object_get(level, "supported")))
            effort_set_add(out, ANTHROPIC_EFFORT_LADDER[i]);
    }

    const char *name;
    json_t *level;
    json_object_foreach(effort, name, level)
    {
        if (strcmp(name, "supported") != 0 && json_is_object(level) &&
            json_is_true(json_object_get(level, "supported"))) {
            effort_set_add(out, name);
        }
    }
}

void anthropic_parse_model(const json_t *entry, struct model_info *out)
{
    json_t *max_input = json_object_get(entry, "max_input_tokens");
    if (json_is_integer(max_input) && json_integer_value(max_input) > 0)
        out->context = (long)json_integer_value(max_input);

    json_t *max_output = json_object_get(entry, "max_tokens");
    if (json_is_integer(max_output) && json_integer_value(max_output) > 0)
        out->max_output = (long)json_integer_value(max_output);

    json_t *capabilities = json_object_get(entry, "capabilities");
    json_t *image = json_object_get(capabilities, "image_input");
    json_t *image_supported = json_object_get(image, "supported");
    if (json_is_boolean(image_supported)) {
        out->image_input = json_is_true(image_supported) ? PROVIDER_CAP_YES : PROVIDER_CAP_NO;
    }

    parse_model_efforts(json_object_get(capabilities, "effort"), &out->efforts);
}

static void parse_model_probe_response(const char *response_body, const char *model,
                                       struct model_info *out)
{
    json_t *root = json_loads(response_body, 0, NULL);
    if (!root)
        return;

    json_t *data = json_object_get(root, "data");
    size_t index;
    json_t *entry;
    json_array_foreach(data, index, entry)
    {
        const char *model_id = json_string_value(json_object_get(entry, "id"));
        if (model_id && strcmp(model_id, model) == 0) {
            anthropic_parse_model(entry, out);
            break;
        }
    }
    json_decref(root);
}

static int anthropic_probe_model(struct provider *provider, const char *model,
                                 struct model_probe *probe)
{
    struct anthropic *anthropic = (struct anthropic *)provider;
    if (!model || !*model)
        return -1;

    probe->url = xasprintf("%s/models?limit=%d", anthropic->base_url, ANTHROPIC_MODEL_PAGE_SIZE);
    probe->headers = build_request_headers(anthropic, 0);
    probe->timeout_s = MODEL_PROBE_TIMEOUT_S;
    probe->parse = parse_model_probe_response;
    return 0;
}

/* The server-provided cursor is inserted into a query parameter without encoding. */
static int cursor_is_safe(const char *cursor)
{
    if (!cursor || !*cursor)
        return 0;
    for (const char *byte = cursor; *byte; byte++) {
        int safe = (*byte >= 'A' && *byte <= 'Z') || (*byte >= 'a' && *byte <= 'z') ||
                   (*byte >= '0' && *byte <= '9') || *byte == '.' || *byte == '_' || *byte == '-';
        if (!safe)
            return 0;
    }
    return 1;
}

/* On success, `page` owns the parsed response. */
static int fetch_models_page(const struct anthropic *anthropic, const char *after_id,
                             http_tick_cb tick, void *tick_user, json_t **page, char **error)
{
    char *url =
        after_id ? xasprintf("%s/models?limit=%d&after_id=%s", anthropic->base_url,
                             ANTHROPIC_MODEL_PAGE_SIZE, after_id)
                 : xasprintf("%s/models?limit=%d", anthropic->base_url, ANTHROPIC_MODEL_PAGE_SIZE);
    char **request_headers = build_request_headers(anthropic, 0);

    char *response_body = NULL;
    long status = 0;
    int result = http_get(url, (const char *const *)request_headers, MODEL_LIST_TIMEOUT_S, 0, tick,
                          tick_user, &response_body, &status);
    string_array_free(request_headers);
    free(url);

    if (result != 0) {
        *error = format_model_list_error(anthropic->base.name, anthropic->base_url,
                                         anthropic->api_key != NULL, status);
        free(response_body);
        return -1;
    }

    *page = json_loads(response_body, 0, NULL);
    free(response_body);
    if (!*page) {
        const char *provider_name = anthropic->base.name ? anthropic->base.name : "provider";
        *error = xasprintf("%s /models response is not valid JSON", provider_name);
        return -1;
    }
    return 0;
}

static void append_models(json_t *data, struct model_info **models, size_t *n_models,
                          size_t *capacity, int *saw_entry)
{
    size_t n_entries = json_array_size(data);
    for (size_t i = 0; i < n_entries; i++) {
        json_t *entry = json_array_get(data, i);
        const char *model_id = json_string_value(json_object_get(entry, "id"));
        *saw_entry = 1;
        if (!model_id || !*model_id)
            continue;

        if (*n_models == *capacity) {
            *capacity = *capacity ? *capacity * 2 : (n_entries > 8 ? n_entries : 8);
            *models = xrealloc(*models, *capacity * sizeof(**models));
        }
        model_info_init(&(*models)[*n_models]);
        (*models)[*n_models].id = xstrdup(model_id);
        anthropic_parse_model(entry, &(*models)[*n_models]);
        (*n_models)++;
    }
}

static int anthropic_list_models(struct provider *provider, struct model_info **models,
                                 size_t *n_models, char **error, http_tick_cb tick, void *tick_user)
{
    struct anthropic *anthropic = (struct anthropic *)provider;
    *models = NULL;
    *n_models = 0;

    struct model_info *available = NULL;
    size_t n_available = 0;
    size_t capacity = 0;
    char *after_id = NULL;
    int saw_entry = 0;

    for (int page_number = 0; page_number < ANTHROPIC_MODEL_PAGE_LIMIT; page_number++) {
        json_t *page = NULL;
        if (fetch_models_page(anthropic, after_id, tick, tick_user, &page, error) != 0)
            goto fail;

        json_t *data = json_object_get(page, "data");
        if (!json_is_array(data) && !json_is_null(data)) {
            json_decref(page);
            const char *name = provider->name ? provider->name : "provider";
            *error = xasprintf("%s /models response has no model list", name);
            goto fail;
        }

        const char *last_id = json_string_value(json_object_get(page, "last_id"));
        /* Discard a repeated page before it can duplicate models already collected. */
        if (after_id && last_id && strcmp(after_id, last_id) == 0) {
            json_decref(page);
            break;
        }

        append_models(data, &available, &n_available, &capacity, &saw_entry);
        int has_more = json_is_true(json_object_get(page, "has_more"));
        free(after_id);
        after_id = has_more && cursor_is_safe(last_id) ? xstrdup(last_id) : NULL;
        json_decref(page);
        if (!after_id)
            break;
    }
    free(after_id);

    if (saw_entry && n_available == 0) {
        const char *name = provider->name ? provider->name : "provider";
        *error = xasprintf("%s /models response contains no usable model ids", name);
        model_info_free(available, n_available);
        return -1;
    }

    *models = available;
    *n_models = n_available;
    return 0;

fail:
    free(after_id);
    model_info_free(available, n_available);
    return -1;
}

static size_t anthropic_list_efforts(struct provider *provider, const char *const **efforts)
{
    struct anthropic *anthropic = (struct anthropic *)provider;
    if (resolve_thinking_mode(anthropic) != ANTHROPIC_THINKING_ADAPTIVE)
        return 0;
    *efforts = ANTHROPIC_EFFORT_LADDER;
    return ANTHROPIC_EFFORT_LADDER_N;
}

static void anthropic_destroy(struct provider *provider)
{
    struct anthropic *anthropic = (struct anthropic *)provider;
    model_meta_release(provider);
    free(anthropic->base_url);
    free(anthropic->api_key);
    free(anthropic->name);
    free(anthropic->catalog_id);
    free(anthropic->endpoint);
    free(anthropic->version);
    free(anthropic->config_prefix);
    string_array_free(anthropic->extra_headers);
    json_decref(anthropic->extra_body);
    free(anthropic);
}

static char *resolve_base_url(const struct anthropic_preset *preset)
{
    const char *configured = config_scoped_str(preset->config_prefix, "base_url");
    if (preset->pin_base_url) {
        /* Silently accepting the key would fake a redirect the pin just refused. */
        if (configured && *configured) {
            hax_warn("provider '%s': base_url is pinned to %s — use a custom provider for "
                     "another endpoint",
                     preset->display_name, preset->default_base_url);
        }
        configured = NULL;
    }
    const char *base_url = configured && *configured ? configured : preset->default_base_url;
    if (!base_url || !*base_url) {
        hax_err("internal: anthropic preset has no base URL");
        return NULL;
    }
    return dup_trim_trailing_slash(base_url);
}

static const char *resolve_display_name(const struct anthropic_preset *preset)
{
    const char *configured = config_scoped_str(preset->config_prefix, "display_name");
    if (configured && *configured)
        return configured;
    if (preset->display_name && *preset->display_name)
        return preset->display_name;
    return "anthropic";
}

struct provider *anthropic_provider_new_preset(const struct anthropic_preset *preset)
{
    const struct anthropic_preset empty = {0};
    if (!preset)
        preset = &empty;

    char *base_url = resolve_base_url(preset);
    if (!base_url)
        return NULL;

    struct anthropic *anthropic = xcalloc(1, sizeof(*anthropic));
    anthropic->base_url = base_url;
    const char *api_key = provider_api_key(preset->config_prefix, preset->api_key_env);
    anthropic->api_key = api_key ? xstrdup(api_key) : NULL;
    anthropic->name = xstrdup(resolve_display_name(preset));
    anthropic->catalog_id = preset->catalog_id ? xstrdup(preset->catalog_id) : NULL;
    anthropic->endpoint = xasprintf("%s/messages", anthropic->base_url);
    const char *version = config_scoped_str(preset->config_prefix, "version");
    anthropic->version = xstrdup(version && *version ? version : ANTHROPIC_DEFAULT_VERSION);
    anthropic->config_prefix = preset->config_prefix ? xstrdup(preset->config_prefix) : NULL;
    anthropic->extra_headers = provider_extra_headers(preset->config_prefix);
    anthropic->extra_body = provider_extra_body(preset->config_prefix);
    anthropic->cache_ttl = provider_cache_ttl(preset->config_prefix);
    anthropic->default_thinking_mode = preset->default_thinking_mode;
    anthropic->allow_empty_signature = preset->allow_empty_signature;
    anthropic->cache_default = preset->send_cache_control_default;

    anthropic->base.name = anthropic->name;
    anthropic->base.catalog_id = anthropic->catalog_id;
    anthropic->base.stream = anthropic_stream;
    anthropic->base.list_models = anthropic_list_models;
    anthropic->base.list_efforts = anthropic_list_efforts;
    anthropic->base.probe_model = anthropic_probe_model;
    anthropic->base.destroy = anthropic_destroy;

    const char *configured_model = config_str("model");
    model_meta_refresh(&anthropic->base,
                       configured_model && *configured_model ? configured_model : NULL);
    return &anthropic->base;
}

struct provider *anthropic_provider_new(const char *id)
{
    provider_warn_unused_fields(id, "anthropic-messages", PROVIDER_FIELD_ANTHROPIC, NULL);
    /* Tweaks resolve from the provider's own block; the pinned endpoint keeps ANTHROPIC_API_KEY
     * from being redirected to a custom URL. */
    struct anthropic_preset preset = {
        .display_name = "anthropic",
        .default_base_url = "https://api.anthropic.com/v1",
        .api_key_env = "ANTHROPIC_API_KEY",
        .config_prefix = "providers.anthropic",
        .pin_base_url = 1,
        .default_thinking_mode = ANTHROPIC_THINKING_ADAPTIVE,
        .allow_empty_signature = 0,
        .send_cache_control_default = 1,
        .catalog_id = "anthropic",
    };
    struct provider *provider = anthropic_provider_new_preset(&preset);
    if (provider)
        provider->id = id;
    return provider;
}

static void anthropic_prepare_availability(const char *id, struct provider_availability *out)
{
    (void)id;
    out->available = provider_api_key("providers.anthropic", "ANTHROPIC_API_KEY") != NULL;
    out->reason = out->available ? NULL : "ANTHROPIC_API_KEY not set";
}

const struct provider_factory PROVIDER_ANTHROPIC = {
    .id = "anthropic",
    .new = anthropic_provider_new,
    .prepare_availability = anthropic_prepare_availability,
};
