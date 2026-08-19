/* SPDX-License-Identifier: MIT */
#include "providers/http_provider.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "catalog.h"
#include "config.h"
#include "model_meta.h"
#include "provider.h"
#include "util.h"
#include "providers/anthropic_messages.h"
#include "providers/config_provider.h"
#include "providers/openai_messages.h"
#include "providers/stream_retry.h"
#include "providers/wire.h"
#include "transport/api_error.h"
#include "transport/http.h"

#define MODEL_LIST_TIMEOUT_S 10

#define MESSAGES_DEFAULT_VERSION    "2023-06-01"
#define MESSAGES_DEFAULT_MAX_TOKENS 32000

struct http_provider {
    struct provider base;
    char *base_url;
    char *api_key;
    char *name;
    char *catalog_id;
    char *endpoint;
    char *config_prefix;
    char *session_id;
    const struct wire *wire;
    char *version; /* anthropic-version; set only on the Messages wire */
    int send_cache_key;
    int emit_progress;
    int request_cost;
    enum openai_cache_mode cache_mode;
    char *cache_ttl;
    char *reasoning_field;
    enum openai_reasoning_format reasoning_format;
    enum anthropic_thinking_mode default_thinking_mode;
    int allow_empty_signature;
    int cache_default; /* Messages cache_control default; chat uses cache_mode */
    char **extra_headers;
    json_t *extra_body;

    const char *length_hint;    /* borrowed for the provider lifetime */
    const char *const *efforts; /* borrowed for the provider lifetime */
    size_t n_efforts;
    void (*parse_model)(const json_t *entry, struct model_info *out);
};

static enum anthropic_thinking_mode resolve_thinking_mode(const struct http_provider *provider)
{
    const char *configured = config_scoped_str(provider->config_prefix, "thinking_mode");
    if (!configured || !*configured)
        return provider->default_thinking_mode;
    if (strcasecmp(configured, "adaptive") == 0)
        return ANTHROPIC_THINKING_ADAPTIVE;
    if (strcasecmp(configured, "budget") == 0)
        return ANTHROPIC_THINKING_BUDGET;
    if (strcasecmp(configured, "off") == 0)
        return ANTHROPIC_THINKING_OFF;

    hax_warn("unknown thinking_mode '%s' (adaptive/budget/off) — using default", configured);
    return provider->default_thinking_mode;
}

int http_provider_max_tokens(struct provider *base, const char *model)
{
    struct http_provider *provider = (struct http_provider *)base;
    int configured = 0;
    int user_set = 0;
    if (provider->config_prefix) {
        char *key = xasprintf("%s.max_tokens", provider->config_prefix);
        configured = config_int(key);
        user_set = strcmp(config_source(key), "default") != 0;
        free(key);
    }

    long model_limit = model_meta_max_output(base, model);
    if (user_set && configured > 0)
        return model_limit > 0 && configured > model_limit ? (int)model_limit : configured;
    if (model_limit > 0)
        return (int)model_limit;
    return configured > 0 ? configured : MESSAGES_DEFAULT_MAX_TOKENS;
}

/* Owned NULL-terminated headers; free with string_array_free. The auth scheme follows the wire:
 * Bearer for the OpenAI family, x-api-key plus the version header for Messages. Streaming
 * requests add the SSE Accept and the JSON Content-Type. */
static char **build_headers(const struct http_provider *provider, int streaming)
{
    char *auth = NULL;
    char *version = NULL;
    if (provider->wire == &WIRE_ANTHROPIC_MESSAGES) {
        if (provider->api_key)
            auth = xasprintf("x-api-key: %s", provider->api_key);
        version = xasprintf("anthropic-version: %s", provider->version);
    } else if (provider->api_key) {
        auth = xasprintf("Authorization: Bearer %s", provider->api_key);
    }

    const char *fixed[5];
    size_t n_fixed = 0;
    if (auth)
        fixed[n_fixed++] = auth;
    if (version)
        fixed[n_fixed++] = version;
    if (streaming) {
        fixed[n_fixed++] = "Accept: text/event-stream";
        fixed[n_fixed++] = "Content-Type: application/json";
    }
    fixed[n_fixed] = NULL;

    char **headers = string_array_concat(fixed, (const char *const *)provider->extra_headers);
    free(auth);
    free(version);
    return headers;
}

struct http_stream {
    const struct http_provider *provider;
    struct openai_cache_plan cache;
    union wire_events events;
};

static char **stream_build_headers(void *ctx)
{
    return build_headers(((struct http_stream *)ctx)->provider, 1);
}

static void stream_parser_init(void *ctx, stream_cb callback, void *callback_user)
{
    struct http_stream *stream = ctx;
    struct wire_events_opts opts = {
        .emit_progress = stream->provider->emit_progress,
        .length_hint = stream->provider->length_hint,
        .cache_write_1h = stream->cache.writes_bill_1h,
    };
    stream->provider->wire->events_init(&stream->events, callback, callback_user, &opts);
}

static int handle_sse_data(const char *event_name, const char *data, void *user)
{
    struct http_stream *stream = user;
    stream->provider->wire->events_feed(&stream->events, event_name, data);
    return 0;
}

static void stream_parser_finalize(void *ctx)
{
    struct http_stream *stream = ctx;
    stream->provider->wire->events_finalize(&stream->events);
}

static void stream_parser_free(void *ctx)
{
    struct http_stream *stream = ctx;
    stream->provider->wire->events_free(&stream->events);
}

static int http_provider_stream(struct provider *base, const struct context *context,
                                const char *model, stream_cb callback, void *callback_user,
                                http_tick_cb tick, void *tick_user)
{
    struct http_provider *provider = (struct http_provider *)base;
    struct http_stream stream = {.provider = provider};
    struct wire_body_opts opts = {
        .extra_body = provider->extra_body,
        .cache_ttl = provider->cache_ttl,
    };

    if (provider->wire == &WIRE_ANTHROPIC_MESSAGES) {
        opts.cache_markers =
            config_scoped_bool_or(provider->config_prefix, "cache", provider->cache_default);
        opts.max_tokens = http_provider_max_tokens(base, model);
        opts.thinking_mode = resolve_thinking_mode(provider);
        opts.thinking_budget = config_scoped_int(provider->config_prefix, "thinking_budget");
        opts.show_reasoning = config_bool("show_reasoning");
        opts.allow_empty_signature = provider->allow_empty_signature;
    } else {
        /* Cache planning depends on rates populated by the startup metadata probe. Bounded: a
         * router-autoload probe can take minutes, while rate-reporting probes answer quickly,
         * and the request itself waits for the model to load anyway. */
        model_meta_wait_ms(base, MODEL_META_PROBE_WAIT_MS);
        struct catalog_entry rates;
        model_meta_rates(base, model, &rates);
        stream.cache = openai_plan_cache(&rates, provider->cache_mode, provider->cache_ttl);
        opts.cache_markers = stream.cache.send_breakpoints;
        opts.session_cache_key = provider->send_cache_key ? provider->session_id : NULL;
        opts.reasoning_field = provider->reasoning_field;
        opts.reasoning_format = provider->reasoning_format;
        opts.emit_progress = provider->emit_progress;
        opts.request_cost = provider->request_cost;
    }

    char *body = wire_build_body(provider->wire, context, provider_stable_id(base), model, &opts);
    if (!body)
        return -1;

    struct stream_retry request = {
        .endpoint = provider->endpoint,
        .body = body,
        .body_len = strlen(body),
        .ctx = &stream,
        .build_headers = stream_build_headers,
        .parser_init = stream_parser_init,
        .parser_feed = handle_sse_data,
        .parser_finalize = stream_parser_finalize,
        .parser_free = stream_parser_free,
    };
    int result = stream_retry_run(&request, callback, callback_user, tick, tick_user);
    free(body);
    return result;
}

static void http_provider_destroy(struct provider *base)
{
    struct http_provider *provider = (struct http_provider *)base;
    model_meta_release(base);
    free(provider->base_url);
    free(provider->api_key);
    free(provider->name);
    free(provider->catalog_id);
    free(provider->endpoint);
    free(provider->config_prefix);
    free(provider->session_id);
    free(provider->version);
    free(provider->cache_ttl);
    free(provider->reasoning_field);
    string_array_free(provider->extra_headers);
    json_decref(provider->extra_body);
    free(provider);
}

/* The Messages wire is pinned: its knobs, auth scheme, and metadata protocol differ, so
 * <prefix>.api cannot move a provider across wire families, only between the two OpenAI
 * protocols. */
static const struct wire *resolve_wire(const struct http_provider_preset *preset)
{
    const struct wire *fallback = preset->wire ? preset->wire : &WIRE_OPENAI_CHAT;
    if (fallback == &WIRE_ANTHROPIC_MESSAGES)
        return fallback;

    const char *api = config_scoped_str(preset->config_prefix, "api");
    if (!api || !*api)
        return fallback;

    const struct wire *wire = wire_find(api);
    if (!wire || wire == &WIRE_ANTHROPIC_MESSAGES) {
        hax_warn("unknown api %s (expected 'chat' or 'responses') — using default", api);
        return fallback;
    }
    return wire;
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

static int http_provider_list_models(struct provider *base, struct model_info **models,
                                     size_t *n_models, char **error, http_tick_cb tick,
                                     void *tick_user)
{
    struct http_provider *provider = (struct http_provider *)base;
    *models = NULL;
    *n_models = 0;

    char *url = xasprintf("%s/models", provider->base_url);
    char **headers = build_headers(provider, 0);

    char *response_body = NULL;
    long status = 0;
    int result = http_get(url, (const char *const *)headers, MODEL_LIST_TIMEOUT_S, 0, tick,
                          tick_user, &response_body, &status);
    string_array_free(headers);
    free(url);

    if (result != 0) {
        *error = format_model_list_error(base->name, provider->base_url, provider->api_key != NULL,
                                         status);
        free(response_body);
        return -1;
    }

    json_t *root = json_loads(response_body, 0, NULL);
    free(response_body);
    const char *provider_name = base->name ? base->name : "provider";
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
        if (provider->parse_model)
            provider->parse_model(entry, &available[n_available]);
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

static size_t http_provider_list_efforts(struct provider *base, const char *const **efforts)
{
    struct http_provider *provider = (struct http_provider *)base;
    if (!provider->efforts || provider->n_efforts == 0)
        return 0;
    /* Messages effort levels steer adaptive thinking only; other modes take none. */
    if (provider->wire == &WIRE_ANTHROPIC_MESSAGES &&
        resolve_thinking_mode(provider) != ANTHROPIC_THINKING_ADAPTIVE)
        return 0;
    *efforts = provider->efforts;
    return provider->n_efforts;
}

const char *http_provider_base_url(const struct provider *provider)
{
    return ((const struct http_provider *)provider)->base_url;
}

int http_provider_has_api_key(const struct provider *provider)
{
    return ((const struct http_provider *)provider)->api_key != NULL;
}

char **http_provider_metadata_headers(const struct provider *provider)
{
    return build_headers((const struct http_provider *)provider, 0);
}

void http_provider_prepare_base_url_availability(const char *base_url, const char *api_key,
                                                 char *const *extra_headers,
                                                 struct provider_availability *out)
{
    out->available = 0;
    out->reason = "server not reachable";
    out->url = xasprintf("%s/models", base_url);
    out->timeout_s = 2;
    char *authorization =
        api_key && *api_key ? xasprintf("Authorization: Bearer %s", api_key) : NULL;
    const char *fixed[] = {authorization, NULL};
    out->headers = string_array_concat(fixed, (const char *const *)extra_headers);
    free(authorization);
}

static char *resolve_base_url(const struct http_provider_preset *preset)
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
        hax_err("internal: provider preset has no base URL");
        return NULL;
    }
    return dup_trim_trailing_slash(base_url);
}

static const char *resolve_display_name(const struct http_provider_preset *preset)
{
    const char *configured = config_scoped_str(preset->config_prefix, "display_name");
    if (configured && *configured)
        return configured;
    if (preset->display_name && *preset->display_name)
        return preset->display_name;
    return "provider";
}

struct provider *http_provider_new_preset(const struct http_provider_preset *preset)
{
    const struct http_provider_preset empty = {0};
    if (!preset)
        preset = &empty;

    char *base_url = resolve_base_url(preset);
    if (!base_url)
        return NULL;

    struct http_provider *provider = xcalloc(1, sizeof(*provider));
    provider->base_url = base_url;
    const char *api_key = provider_api_key(preset->config_prefix, preset->api_key_env);
    provider->api_key = api_key ? xstrdup(api_key) : NULL;
    provider->name = xstrdup(resolve_display_name(preset));
    provider->catalog_id = preset->catalog_id ? xstrdup(preset->catalog_id) : NULL;
    provider->config_prefix = preset->config_prefix ? xstrdup(preset->config_prefix) : NULL;
    provider->wire = resolve_wire(preset);
    provider->endpoint = xasprintf("%s%s", provider->base_url, provider->wire->path);
    if (provider->wire == &WIRE_ANTHROPIC_MESSAGES) {
        const char *version = config_scoped_str(preset->config_prefix, "version");
        provider->version = xstrdup(version && *version ? version : MESSAGES_DEFAULT_VERSION);
    }

    provider->send_cache_key = config_scoped_bool_or(preset->config_prefix, "send_cache_key",
                                                     preset->send_cache_key_default);
    provider->emit_progress = preset->emit_progress;
    provider->request_cost =
        config_scoped_bool_or(preset->config_prefix, "request_cost", preset->request_cost);
    provider->cache_mode = resolve_cache_mode(preset->config_prefix, preset->cache_auto_default);
    provider->cache_ttl = xstrdup(provider_cache_ttl(preset->config_prefix));
    provider->reasoning_field =
        resolve_reasoning_field(preset->config_prefix, preset->reasoning_replay_field);
    provider->reasoning_format = openai_reasoning_format_parse(
        config_scoped_str(preset->config_prefix, "reasoning_format"), preset->reasoning_format);
    provider->default_thinking_mode = preset->default_thinking_mode;
    provider->allow_empty_signature = preset->allow_empty_signature;
    provider->cache_default = preset->send_cache_control_default;
    /* Preset headers first, then config-declared ones; both reach every request. */
    char **config_headers = provider_extra_headers(preset->config_prefix);
    provider->extra_headers =
        string_array_concat(preset->extra_headers, (const char *const *)config_headers);
    string_array_free(config_headers);
    provider->extra_body = provider_extra_body(preset->config_prefix);

    provider->length_hint = preset->length_hint;
    provider->efforts = preset->efforts;
    provider->n_efforts = preset->n_efforts;
    provider->parse_model = preset->parse_model;

    char session_id[37];
    gen_uuid_v4(session_id);
    provider->session_id = xstrdup(session_id);

    provider->base.name = provider->name;
    provider->base.catalog_id = provider->catalog_id;
    provider->base.stream = http_provider_stream;
    provider->base.list_models = http_provider_list_models;
    provider->base.list_efforts = http_provider_list_efforts;
    provider->base.destroy = http_provider_destroy;
    return &provider->base;
}
