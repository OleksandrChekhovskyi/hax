/* SPDX-License-Identifier: MIT */
#include "providers/openai.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "catalog.h"
#include "config.h"
#include "model_meta.h"
#include "provider.h"
#include "tool_schema.h"
#include "util.h"
#include "providers/openai_events.h"
#include "providers/openai_messages.h"
#include "transport/api_error.h"
#include "transport/http.h"
#include "transport/retry.h"

#define AVAILABILITY_TIMEOUT_S 2
#define MODEL_LIST_TIMEOUT_S   10

const char *const OPENAI_EFFORT_LADDER[] = {"none", "minimal", "low", "medium",
                                            "high", "xhigh",   "max"};
const size_t OPENAI_EFFORT_LADDER_N =
    sizeof(OPENAI_EFFORT_LADDER) / sizeof(OPENAI_EFFORT_LADDER[0]);

struct openai {
    struct provider base;
    char *base_url;
    char *api_key;
    char *name;
    char *catalog_id;
    char *endpoint;
    char *session_id;
    int send_cache_key;
    int emit_progress;
    int request_cost;
    enum openai_cache_mode cache_mode;
    char *cache_ttl;
    char *reasoning_field;
    enum openai_reasoning_format reasoning_format;
    char **extra_headers;

    const char *length_hint;    /* borrowed for the provider lifetime */
    const char *const *efforts; /* borrowed for the provider lifetime */
    size_t n_efforts;
    void (*parse_model)(const json_t *entry, struct model_info *out);
};

/* AUTO sends explicit cache markers only when writes replace ordinary input processing. */
struct openai_cache_plan openai_plan_cache(const struct provider *provider, const char *model,
                                           enum openai_cache_mode mode, const char *ttl)
{
    struct openai_cache_plan plan = {0};
    struct catalog_entry rates;
    model_meta_rates(provider, model, &rates);
    plan.send_breakpoints = mode == OPENAI_CACHE_ON || (mode == OPENAI_CACHE_AUTO &&
                                                        catalog_cache_write_replaces_input(&rates));
    plan.writes_bill_1h = plan.send_breakpoints && ttl && strcasecmp(ttl, "1h") == 0 &&
                          rates.cost_cache_write_1h >= 0;
    return plan;
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

static char *build_request_body(const struct openai *openai, const struct context *context,
                                const char *model, const struct openai_cache_plan *cache)
{
    json_t *messages = openai_build_messages(context->system_prompt, context->items,
                                             context->n_items, openai->reasoning_field,
                                             openai->base.name, model, context->image_input);
    if (cache->send_breakpoints)
        openai_apply_cache_breakpoints(messages, openai->cache_ttl);

    /* Usage is requested on every stream so terminal events can report token counts. */
    json_t *body = json_pack("{s:s, s:b, s:o, s:{s:b}}", "model", model, "stream", 1, "messages",
                             messages, "stream_options", "include_usage", 1);

    if (context->n_tools > 0)
        json_object_set_new(body, "tools", build_tools(context->tools, context->n_tools));
    if (openai->send_cache_key)
        json_object_set_new(body, "prompt_cache_key", json_string(openai->session_id));
    if (openai->emit_progress)
        json_object_set_new(body, "return_progress", json_true());
    if (openai->request_cost)
        json_object_set_new(body, "usage", json_pack("{s:b}", "include", 1));

    openai_apply_reasoning(body, openai->reasoning_format, context->effort);

    char *json = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    return json;
}

static const char **build_request_headers(const struct openai *openai, char **authorization)
{
    size_t n_extra_headers = 0;
    for (char **header = openai->extra_headers; header && *header; header++)
        n_extra_headers++;

    *authorization =
        openai->api_key ? xasprintf("Authorization: Bearer %s", openai->api_key) : NULL;
    const char **headers = xmalloc(sizeof(*headers) * (n_extra_headers + 4));
    size_t n_headers = 0;
    if (*authorization)
        headers[n_headers++] = *authorization;
    headers[n_headers++] = "Accept: text/event-stream";
    headers[n_headers++] = "Content-Type: application/json";
    for (char **header = openai->extra_headers; header && *header; header++)
        headers[n_headers++] = *header;
    headers[n_headers] = NULL;
    return headers;
}

static int handle_sse_data(const char *event_name, const char *data, void *user)
{
    (void)event_name;
    openai_events_feed(user, data);
    return 0;
}

static int openai_stream(struct provider *provider, const struct context *context,
                         const char *model, stream_cb callback, void *callback_user,
                         http_tick_cb tick, void *tick_user)
{
    struct openai *openai = (struct openai *)provider;

    /* Cache planning depends on rates populated by the startup metadata probe. */
    model_meta_wait(provider);
    struct openai_cache_plan cache =
        openai_plan_cache(provider, model, openai->cache_mode, openai->cache_ttl);

    char *body = build_request_body(openai, context, model, &cache);
    if (!body)
        return -1;

    size_t body_len = strlen(body);
    char *authorization;
    const char **headers = build_request_headers(openai, &authorization);
    struct retry_policy policy = retry_policy_default();
    struct http_response response;
    struct openai_events parser;
    int result = -1;

    /* Each retry needs fresh parser state; request bytes remain safe to resend unchanged. */
    for (int attempt = 0; attempt < policy.max_attempts; attempt++) {
        memset(&response, 0, sizeof(response));
        openai_events_init(&parser, callback, callback_user);
        parser.emit_progress = openai->emit_progress;
        parser.length_hint = openai->length_hint;
        parser.cache_write_1h = cache.writes_bill_1h;

        result = http_sse_post(openai->endpoint, headers, body, body_len, policy.idle_timeout_s,
                               handle_sse_data, &parser, tick, tick_user, &response);
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
        openai_events_free(&parser);

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
            openai_events_finalize(&parser);
        }
    }

    free(response.error_body);
    openai_events_free(&parser);
    free(headers);
    free(authorization);
    free(body);
    return result;
}

static void openai_destroy(struct provider *provider)
{
    struct openai *openai = (struct openai *)provider;
    model_meta_release(provider);
    free(openai->base_url);
    free(openai->api_key);
    free(openai->name);
    free(openai->catalog_id);
    free(openai->endpoint);
    free(openai->session_id);
    free(openai->cache_ttl);
    free(openai->reasoning_field);
    string_array_free(openai->extra_headers);
    free(openai);
}

static char **duplicate_headers(const char *const *headers)
{
    if (!headers || !*headers)
        return NULL;

    size_t n_headers = 0;
    while (headers[n_headers])
        n_headers++;

    char **copy = xmalloc(sizeof(*copy) * (n_headers + 1));
    for (size_t i = 0; i < n_headers; i++)
        copy[i] = xstrdup(headers[i]);
    copy[n_headers] = NULL;
    return copy;
}

static char *make_config_key(const char *prefix, const char *leaf)
{
    return xasprintf("%s.%s", prefix ? prefix : "openai", leaf);
}

static const char *preset_config(const char *prefix, const char *leaf)
{
    char *key = make_config_key(prefix, leaf);
    const char *value = config_str(key);
    free(key);
    return value;
}

static int preset_bool(const char *prefix, const char *leaf, int fallback)
{
    char *key = make_config_key(prefix, leaf);
    int value = config_bool_or(key, fallback);
    free(key);
    return value;
}

static enum openai_cache_mode resolve_cache_mode(const char *prefix, int automatic)
{
    char *key = make_config_key(prefix, "cache");

    /* Different fallbacks distinguish a parsed boolean from auto, unset, or invalid input. */
    int with_false_fallback = config_bool_or(key, 0);
    int with_true_fallback = config_bool_or(key, 1);
    free(key);

    if (with_false_fallback == with_true_fallback)
        return with_true_fallback ? OPENAI_CACHE_ON : OPENAI_CACHE_OFF;
    return automatic ? OPENAI_CACHE_AUTO : OPENAI_CACHE_OFF;
}

static char *resolve_reasoning_field(const char *prefix, const char *preset_default)
{
    const char *configured = preset_config(prefix, "reasoning_roundtrip");
    const char *field = preset_default;
    if (configured) {
        if (!*configured || strcmp(configured, "off") == 0 || strcmp(configured, "0") == 0)
            field = NULL;
        else if (strcmp(configured, "on") == 0 || strcmp(configured, "1") == 0)
            field = "reasoning_content";
        else
            field = configured;
    }
    return field ? xstrdup(field) : NULL;
}

static int openai_list_models(struct provider *provider, struct model_info **models,
                              size_t *n_models, char **error, http_tick_cb tick, void *tick_user)
{
    struct openai *openai = (struct openai *)provider;
    *models = NULL;
    *n_models = 0;

    char *url = xasprintf("%s/models", openai->base_url);
    char *authorization =
        openai->api_key ? xasprintf("Authorization: Bearer %s", openai->api_key) : NULL;
    const char *headers[] = {authorization, NULL};
    char *response_body = NULL;
    long status = 0;
    int result = http_get(url, authorization ? headers : NULL, MODEL_LIST_TIMEOUT_S, 0, tick,
                          tick_user, &response_body, &status);
    free(authorization);
    free(url);

    if (result != 0) {
        *error =
            format_models_error(provider->name, openai->base_url, openai->api_key != NULL, status);
        free(response_body);
        return -1;
    }

    json_t *root = json_loads(response_body, 0, NULL);
    free(response_body);
    const char *provider_name = provider->name ? provider->name : "provider";
    if (!root) {
        *error = xasprintf("%s /models response is not valid JSON", provider_name);
        return -1;
    }

    json_t *data = json_object_get(root, "data");
    /* Ollama reports data:null when the server is reachable but has no models. */
    if (json_is_null(data) || (json_is_array(data) && json_array_size(data) == 0)) {
        json_decref(root);
        return 0;
    }
    if (!json_is_array(data)) {
        json_decref(root);
        *error = xasprintf("%s /models response has no model list", provider_name);
        return -1;
    }

    size_t n_entries = json_array_size(data);
    struct model_info *available = xmalloc(n_entries * sizeof(*available));
    size_t n_available = 0;
    for (size_t i = 0; i < n_entries; i++) {
        json_t *entry = json_array_get(data, i);
        const char *model_id = json_string_value(json_object_get(entry, "id"));
        if (!model_id || !*model_id)
            continue;

        model_info_init(&available[n_available]);
        available[n_available].id = xstrdup(model_id);
        if (openai->parse_model)
            openai->parse_model(entry, &available[n_available]);
        n_available++;
    }
    json_decref(root);

    if (n_available == 0) {
        free(available);
        *error = xasprintf("%s /models response contains no usable model ids", provider_name);
        return -1;
    }

    *models = available;
    *n_models = n_available;
    return 0;
}

static size_t openai_list_efforts(struct provider *provider, const char *const **efforts)
{
    struct openai *openai = (struct openai *)provider;
    if (!openai->efforts || openai->n_efforts == 0)
        return 0;
    *efforts = openai->efforts;
    return openai->n_efforts;
}

int openai_key_available(const char *api_key_env, const char *missing_reason, const char **reason)
{
    const char *key = config_str("openai.api_key");
    if (key && *key)
        return 1;
    if (api_key_env) {
        key = getenv(api_key_env);
        if (key && *key)
            return 1;
    }
    if (reason)
        *reason = missing_reason ? missing_reason : "no API key";
    return 0;
}

void openai_prepare_base_url_availability(const char *base_url, const char *api_key,
                                          struct provider_availability *out)
{
    out->available = 0;
    out->reason = "server not reachable";
    out->url = xasprintf("%s/models", base_url);
    out->timeout_s = AVAILABILITY_TIMEOUT_S;
    if (api_key && *api_key) {
        out->headers = xcalloc(2, sizeof(*out->headers));
        out->headers[0] = xasprintf("Authorization: Bearer %s", api_key);
    }
}

static void openai_prepare_availability(const char *name, struct provider_availability *out)
{
    (void)name;
    const char *reason = NULL;
    out->available = openai_key_available("OPENAI_API_KEY", "OPENAI_API_KEY not set", &reason);
    out->reason = reason;
}

static char *resolve_base_url(const struct openai_preset *preset)
{
    const char *configured =
        preset->lock_base_url ? NULL : preset_config(preset->config_prefix, "base_url");
    const char *base_url = configured && *configured ? configured : preset->default_base_url;
    if (!base_url || !*base_url) {
        hax_err("internal: openai preset has no base URL");
        return NULL;
    }
    return dup_trim_trailing_slash(base_url);
}

static const char *resolve_api_key(const struct openai_preset *preset)
{
    const char *api_key = preset_config(preset->config_prefix, "api_key");
    if ((!api_key || !*api_key) && preset->api_key_env)
        api_key = getenv(preset->api_key_env);
    return api_key;
}

static const char *resolve_display_name(const struct openai_preset *preset)
{
    const char *name = preset->config_prefix ? NULL : config_str("provider_name");
    if (name && *name)
        return name;
    if (preset->display_name && *preset->display_name)
        return preset->display_name;
    return "openai";
}

struct provider *openai_provider_new_preset(const struct openai_preset *preset)
{
    const struct openai_preset empty = {0};
    if (!preset)
        preset = &empty;

    char *base_url = resolve_base_url(preset);
    if (!base_url)
        return NULL;

    struct openai *openai = xcalloc(1, sizeof(*openai));
    openai->base_url = base_url;
    const char *api_key = resolve_api_key(preset);
    openai->api_key = api_key && *api_key ? xstrdup(api_key) : NULL;
    openai->name = xstrdup(resolve_display_name(preset));
    openai->catalog_id = preset->catalog_id ? xstrdup(preset->catalog_id) : NULL;
    openai->endpoint = xasprintf("%s/chat/completions", openai->base_url);

    openai->send_cache_key =
        preset_bool(preset->config_prefix, "send_cache_key", preset->send_cache_key_default);
    openai->emit_progress = preset->emit_progress;
    openai->request_cost = preset_bool(preset->config_prefix, "request_cost", preset->request_cost);
    openai->cache_mode = resolve_cache_mode(preset->config_prefix, preset->cache_auto_default);
    const char *cache_ttl = preset_config(preset->config_prefix, "cache_ttl");
    openai->cache_ttl = xstrdup(cache_ttl ? cache_ttl : "");
    openai->reasoning_field =
        resolve_reasoning_field(preset->config_prefix, preset->reasoning_replay_field);
    openai->reasoning_format = preset->reasoning_format;
    openai->extra_headers = duplicate_headers(preset->extra_headers);

    openai->length_hint = preset->length_hint;
    openai->efforts = preset->efforts;
    openai->n_efforts = preset->n_efforts;
    openai->parse_model = preset->parse_model;

    char session_id[37];
    gen_uuid_v4(session_id);
    openai->session_id = xstrdup(session_id);

    openai->base.name = openai->name;
    openai->base.catalog_id = openai->catalog_id;
    openai->base.stream = openai_stream;
    openai->base.list_models = openai_list_models;
    openai->base.list_efforts = openai_list_efforts;
    openai->base.destroy = openai_destroy;
    return &openai->base;
}

struct provider *openai_provider_new(const char *name)
{
    (void)name;
    /* Lock the host so OPENAI_API_KEY and OpenAI-specific defaults cannot reach a custom URL. */
    struct openai_preset preset = {
        .display_name = "openai",
        .default_base_url = "https://api.openai.com/v1",
        .api_key_env = "OPENAI_API_KEY",
        .send_cache_key_default = 1,
        .lock_base_url = 1,
        .catalog_id = "openai",
        .efforts = OPENAI_EFFORT_LADDER,
        .n_efforts = OPENAI_EFFORT_LADDER_N,
    };
    struct provider *provider = openai_provider_new_preset(&preset);
    if (provider)
        provider->sort_models = 1;
    return provider;
}

const struct provider_factory PROVIDER_OPENAI = {
    .name = "openai",
    .new = openai_provider_new,
    .prepare_availability = openai_prepare_availability,
};
