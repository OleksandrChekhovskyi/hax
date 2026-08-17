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
#include "providers/config_provider.h"
#include "providers/openai_events.h"
#include "providers/openai_messages.h"
#include "providers/responses_events.h"
#include "providers/responses_messages.h"
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
    enum openai_wire wire;
    char **extra_headers;
    json_t *extra_body;

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

static json_t *build_chat_tools(const struct tool_def *tools, size_t n_tools)
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

static char *build_chat_body(const struct openai *openai, const struct context *context,
                             const char *model, const struct openai_cache_plan *cache)
{
    json_t *messages = openai_build_messages(
        context->system_prompt, context->items, context->n_items, openai->reasoning_field,
        provider_stable_id(&openai->base), model, context->image_input);
    if (cache->send_breakpoints)
        openai_apply_cache_breakpoints(messages, openai->cache_ttl);

    /* Usage is requested on every stream so terminal events can report token counts. */
    json_t *body = json_pack("{s:s, s:b, s:o, s:{s:b}}", "model", model, "stream", 1, "messages",
                             messages, "stream_options", "include_usage", 1);

    if (context->n_tools > 0)
        json_object_set_new(body, "tools", build_chat_tools(context->tools, context->n_tools));
    if (openai->send_cache_key)
        json_object_set_new(body, "prompt_cache_key", json_string(openai->session_id));
    if (openai->emit_progress)
        json_object_set_new(body, "return_progress", json_true());
    if (openai->request_cost)
        json_object_set_new(body, "usage", json_pack("{s:b}", "include", 1));

    openai_apply_reasoning(body, openai->reasoning_format, context->effort);
    provider_extra_body_apply(body, openai->extra_body);

    char *json = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    return json;
}

static char *build_responses_body(const struct openai *openai, const struct context *context,
                                  const char *model)
{
    json_t *body = responses_build_body(context, provider_stable_id(&openai->base), model);
    if (openai->send_cache_key)
        json_object_set_new(body, "prompt_cache_key", json_string(openai->session_id));
    provider_extra_body_apply(body, openai->extra_body);

    char *json = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    return json;
}

/* One parser per wire, behind a common interface so the retry loop stays protocol-agnostic. */
struct openai_parser {
    enum openai_wire wire;
    union {
        struct openai_events chat;
        struct responses_events responses;
    } u;
};

static void parser_init(struct openai_parser *parser, const struct openai *openai,
                        const struct openai_cache_plan *cache, stream_cb callback,
                        void *callback_user)
{
    parser->wire = openai->wire;
    if (parser->wire == OPENAI_WIRE_RESPONSES) {
        responses_events_init(&parser->u.responses, callback, callback_user);
        return;
    }

    openai_events_init(&parser->u.chat, callback, callback_user);
    parser->u.chat.emit_progress = openai->emit_progress;
    parser->u.chat.length_hint = openai->length_hint;
    parser->u.chat.cache_write_1h = cache->writes_bill_1h;
}

static void parser_finalize(struct openai_parser *parser)
{
    if (parser->wire == OPENAI_WIRE_RESPONSES)
        responses_events_finalize(&parser->u.responses);
    else
        openai_events_finalize(&parser->u.chat);
}

static void parser_free(struct openai_parser *parser)
{
    if (parser->wire == OPENAI_WIRE_RESPONSES)
        responses_events_free(&parser->u.responses);
    else
        openai_events_free(&parser->u.chat);
}

/* Owned NULL-terminated stream-request headers; free with string_array_free. */
static char **build_request_headers(const struct openai *openai)
{
    char *authorization =
        openai->api_key ? xasprintf("Authorization: Bearer %s", openai->api_key) : NULL;
    const char *fixed[4];
    size_t n_fixed = 0;
    if (authorization)
        fixed[n_fixed++] = authorization;
    fixed[n_fixed++] = "Accept: text/event-stream";
    fixed[n_fixed++] = "Content-Type: application/json";
    fixed[n_fixed] = NULL;

    char **headers = string_array_concat(fixed, (const char *const *)openai->extra_headers);
    free(authorization);
    return headers;
}

static int handle_sse_data(const char *event_name, const char *data, void *user)
{
    (void)event_name;
    struct openai_parser *parser = user;
    if (parser->wire == OPENAI_WIRE_RESPONSES)
        responses_events_feed(&parser->u.responses, data);
    else
        openai_events_feed(&parser->u.chat, data);
    return 0;
}

static int openai_stream(struct provider *provider, const struct context *context,
                         const char *model, stream_cb callback, void *callback_user,
                         http_tick_cb tick, void *tick_user)
{
    struct openai *openai = (struct openai *)provider;

    /* Cache planning depends on rates populated by the startup metadata probe. Bounded: a
     * router-autoload probe can take minutes, while rate-reporting probes answer quickly, and the
     * request itself waits for the model to load anyway. */
    model_meta_wait_ms(provider, MODEL_META_PROBE_WAIT_MS);
    struct openai_cache_plan cache =
        openai_plan_cache(provider, model, openai->cache_mode, openai->cache_ttl);

    char *body = openai->wire == OPENAI_WIRE_RESPONSES
                     ? build_responses_body(openai, context, model)
                     : build_chat_body(openai, context, model, &cache);
    if (!body)
        return -1;

    size_t body_len = strlen(body);
    char **headers = build_request_headers(openai);
    struct retry_policy policy = retry_policy_default();
    struct http_response response;
    struct openai_parser parser;
    int result = -1;

    /* Each retry needs fresh parser state; request bytes remain safe to resend unchanged. */
    for (int attempt = 0; attempt < policy.max_attempts; attempt++) {
        memset(&response, 0, sizeof(response));
        parser_init(&parser, openai, &cache, callback, callback_user);

        result = http_sse_post(openai->endpoint, (const char *const *)headers, body, body_len,
                               policy.idle_timeout_s, handle_sse_data, &parser, tick, tick_user,
                               &response);
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
        parser_free(&parser);

        if (retry_sleep_with_tick(delay_ms, tick, tick_user)) {
            response.cancelled = 1;
            parser_init(&parser, openai, &cache, callback, callback_user);
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
            parser_finalize(&parser);
        }
    }

    free(response.error_body);
    parser_free(&parser);
    string_array_free(headers);
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
    json_decref(openai->extra_body);
    free(openai);
}

enum openai_wire openai_wire_parse(const char *value, enum openai_wire fallback)
{
    if (!value || !*value)
        return fallback;
    if (strcasecmp(value, "chat") == 0 || strcasecmp(value, "openai-completions") == 0)
        return OPENAI_WIRE_CHAT;
    if (strcasecmp(value, "responses") == 0 || strcasecmp(value, "openai-responses") == 0)
        return OPENAI_WIRE_RESPONSES;

    hax_warn("unknown api %s (expected 'chat' or 'responses') — using default", value);
    return fallback;
}

static enum openai_cache_mode resolve_cache_mode(const char *prefix, int automatic)
{
    /* Different fallbacks distinguish a parsed boolean from auto, unset, or invalid input. */
    int with_false_fallback = config_scoped_bool_or(prefix, "cache", 0);
    int with_true_fallback = config_scoped_bool_or(prefix, "cache", 1);

    if (with_false_fallback == with_true_fallback)
        return with_true_fallback ? OPENAI_CACHE_ON : OPENAI_CACHE_OFF;
    return automatic ? OPENAI_CACHE_AUTO : OPENAI_CACHE_OFF;
}

static char *resolve_reasoning_field(const char *prefix, const char *preset_default)
{
    const char *configured = config_scoped_str(prefix, "reasoning_roundtrip");
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
    const char *fixed[] = {authorization, NULL}; /* {NULL, NULL} counts as empty */
    char **headers = string_array_concat(fixed, (const char *const *)openai->extra_headers);
    free(authorization);

    char *response_body = NULL;
    long status = 0;
    int result = http_get(url, (const char *const *)headers, MODEL_LIST_TIMEOUT_S, 0, tick,
                          tick_user, &response_body, &status);
    string_array_free(headers);
    free(url);

    if (result != 0) {
        *error = format_model_list_error(provider->name, openai->base_url, openai->api_key != NULL,
                                         status);
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

void openai_prepare_base_url_availability(const char *base_url, const char *api_key,
                                          char *const *extra_headers,
                                          struct provider_availability *out)
{
    out->available = 0;
    out->reason = "server not reachable";
    out->url = xasprintf("%s/models", base_url);
    out->timeout_s = AVAILABILITY_TIMEOUT_S;
    char *authorization =
        api_key && *api_key ? xasprintf("Authorization: Bearer %s", api_key) : NULL;
    const char *fixed[] = {authorization, NULL};
    out->headers = string_array_concat(fixed, (const char *const *)extra_headers);
    free(authorization);
}

static void openai_prepare_availability(const char *id, struct provider_availability *out)
{
    (void)id;
    out->available = provider_api_key("providers.openai", "OPENAI_API_KEY") != NULL;
    out->reason = out->available ? NULL : "OPENAI_API_KEY not set";
}

static char *resolve_base_url(const struct openai_preset *preset)
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
        hax_err("internal: openai preset has no base URL");
        return NULL;
    }
    return dup_trim_trailing_slash(base_url);
}

static const char *resolve_display_name(const struct openai_preset *preset)
{
    const char *configured = config_scoped_str(preset->config_prefix, "display_name");
    if (configured && *configured)
        return configured;
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
    const char *api_key = provider_api_key(preset->config_prefix, preset->api_key_env);
    openai->api_key = api_key ? xstrdup(api_key) : NULL;
    openai->name = xstrdup(resolve_display_name(preset));
    openai->catalog_id = preset->catalog_id ? xstrdup(preset->catalog_id) : NULL;
    openai->wire = openai_wire_parse(config_scoped_str(preset->config_prefix, "api"), preset->wire);
    openai->endpoint =
        xasprintf("%s/%s", openai->base_url,
                  openai->wire == OPENAI_WIRE_RESPONSES ? "responses" : "chat/completions");

    openai->send_cache_key = config_scoped_bool_or(preset->config_prefix, "send_cache_key",
                                                   preset->send_cache_key_default);
    openai->emit_progress = preset->emit_progress;
    openai->request_cost =
        config_scoped_bool_or(preset->config_prefix, "request_cost", preset->request_cost);
    openai->cache_mode = resolve_cache_mode(preset->config_prefix, preset->cache_auto_default);
    openai->cache_ttl = xstrdup(provider_cache_ttl(preset->config_prefix));
    openai->reasoning_field =
        resolve_reasoning_field(preset->config_prefix, preset->reasoning_replay_field);
    openai->reasoning_format = openai_reasoning_format_parse(
        config_scoped_str(preset->config_prefix, "reasoning_format"), preset->reasoning_format);
    /* Preset headers first, then config-declared ones; both reach every request. */
    char **config_headers = provider_extra_headers(preset->config_prefix);
    openai->extra_headers =
        string_array_concat(preset->extra_headers, (const char *const *)config_headers);
    string_array_free(config_headers);
    openai->extra_body = provider_extra_body(preset->config_prefix);

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

struct provider *openai_provider_new(const char *id)
{
    provider_warn_unused_openai_fields(id, OPENAI_WIRE_RESPONSES, NULL);
    /* Tweaks resolve from the provider's own block; the pinned endpoint keeps OPENAI_API_KEY
     * from being redirected to a custom URL. */
    struct openai_preset preset = {
        .display_name = "openai",
        .default_base_url = "https://api.openai.com/v1",
        .api_key_env = "OPENAI_API_KEY",
        .config_prefix = "providers.openai",
        .pin_base_url = 1,
        .send_cache_key_default = 1,
        .wire = OPENAI_WIRE_RESPONSES,
        .catalog_id = "openai",
        .efforts = OPENAI_EFFORT_LADDER,
        .n_efforts = OPENAI_EFFORT_LADDER_N,
    };
    struct provider *provider = openai_provider_new_preset(&preset);
    if (provider) {
        provider->id = id;
        provider->sort_models = 1;
    }
    return provider;
}

const struct provider_factory PROVIDER_OPENAI = {
    .id = "openai",
    .new = openai_provider_new,
    .prepare_availability = openai_prepare_availability,
};
