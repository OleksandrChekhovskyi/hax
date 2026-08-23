/* SPDX-License-Identifier: MIT */
#include "providers/http_provider.h"

#include <fnmatch.h>
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "catalog.h"
#include "config.h"
#include "model_meta.h"
#include "provider.h"
#include "util.h"
#include "providers/anthropic_body.h"
#include "providers/chat_body.h"
#include "providers/config_provider.h"
#include "providers/stream_retry.h"
#include "providers/vertex_auth.h"
#include "providers/wire.h"
#include "transport/api_error.h"
#include "transport/http.h"

#define MODEL_LIST_TIMEOUT_S 10

#define MESSAGES_DEFAULT_VERSION    "2023-06-01"
#define MESSAGES_DEFAULT_MAX_TOKENS 32000

/* One <prefix>.model_apis member: models matching the glob speak `wire`. */
struct wire_rule {
    char *pattern;
    const struct wire *wire;
};

struct http_provider {
    struct provider base;
    char *base_url;
    char *api_key;
    char *name;
    char *catalog_id;
    char *endpoint; /* for the default wire; other wires derive theirs per request */
    char *config_prefix;
    char *session_id;
    const struct wire *wire; /* default; wire_rules and catalog hints override per model */
    struct wire_rule *wire_rules;
    size_t n_wire_rules;
    int catalog_wires;
    char *version; /* anthropic-version; sent on Messages requests */
    int send_cache_key;
    int emit_progress;
    int request_cost;
    enum chat_cache_mode cache_mode;
    char *cache_ttl;
    char *reasoning_field;
    int reasoning_field_pinned; /* configured explicitly; no catalog hint may override it */
    enum chat_reasoning_format reasoning_format;
    enum anthropic_thinking_mode default_thinking_mode;
    int allow_empty_signature;
    int cache_default; /* Messages cache_control default; chat uses cache_mode */
    int raw_endpoint;  /* base_url is the complete stream endpoint; no wire path is appended */
    const char *(*bearer_token)(const struct provider *provider); /* Messages; NULL → api_key */
    char **extra_headers;
    json_t *extra_body;

    const char *length_hint;    /* borrowed for the provider lifetime */
    const char *const *efforts; /* borrowed for the provider lifetime */
    size_t n_efforts;
    void (*parse_model)(const json_t *entry, struct model_info *out);
};

static enum anthropic_thinking_mode resolve_thinking_mode(const struct http_provider *provider,
                                                          const char *effort)
{
    const char *configured = config_scoped_str(provider->config_prefix, "thinking_mode");
    if (configured && *configured) {
        if (strcasecmp(configured, "adaptive") == 0)
            return ANTHROPIC_THINKING_ADAPTIVE;
        if (strcasecmp(configured, "budget") == 0)
            return ANTHROPIC_THINKING_BUDGET;
        if (strcasecmp(configured, "off") == 0)
            return ANTHROPIC_THINKING_OFF;
        hax_warn("unknown thinking_mode '%s' (adaptive/budget/off) — using default", configured);
    }
    /* An effort reaches a request only when the model's metadata accepts it, and on Messages
     * efforts steer adaptive thinking; a compat-safe budget default must not drop it. */
    if (effort && *effort)
        return ANTHROPIC_THINKING_ADAPTIVE;
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
static char **build_headers(const struct http_provider *provider, const struct wire *wire,
                            int streaming)
{
    char *auth = NULL;
    char *version = NULL;
    if (wire == &WIRE_ANTHROPIC_MESSAGES) {
        const char *token = provider->bearer_token ? provider->bearer_token(&provider->base) : NULL;
        if (token && *token)
            auth = xasprintf("Authorization: Bearer %s", token);
        else if (provider->api_key)
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
    const struct wire *wire; /* resolved for this request's model */
    struct chat_cache_plan cache;
    union wire_events events;
    int auth_retried; /* one in-place retry after a rejected bearer token */
};

static char **stream_build_headers(void *ctx)
{
    struct http_stream *stream = ctx;
    return build_headers(stream->provider, stream->wire, 1);
}

static void stream_parser_init(void *ctx, stream_cb callback, void *callback_user)
{
    struct http_stream *stream = ctx;
    struct wire_events_opts opts = {
        .emit_progress = stream->provider->emit_progress,
        .length_hint = stream->provider->length_hint,
        .cache_write_1h = stream->cache.writes_bill_1h,
    };
    stream->wire->events_init(&stream->events, callback, callback_user, &opts);
}

static int handle_sse_data(const char *event_name, const char *data, void *user)
{
    struct http_stream *stream = user;
    stream->wire->events_feed(&stream->events, event_name, data);
    return 0;
}

static void stream_parser_finalize(void *ctx)
{
    struct http_stream *stream = ctx;
    stream->wire->events_finalize(&stream->events);
}

static void stream_parser_free(void *ctx)
{
    struct http_stream *stream = ctx;
    stream->wire->events_free(&stream->events);
}

/* A 401 on a Bearer-authenticated request (Vertex): the cached token may be rejected (expired
 * access token, rotated refresh). Drop it and retry once so the next build_headers re-resolves.
 * Bounded to a single recovery per stream; a second 401 surfaces as an error rather than
 * spinning on a bad credential. */
static int stream_recover(void *ctx, long http_status, http_tick_cb tick, void *tick_user)
{
    struct http_stream *stream = ctx;
    if (http_status != 401 || stream->auth_retried)
        return 0;
    if (!stream->provider->bearer_token)
        return 0;
    stream->auth_retried = 1;
    vertex_auth_invalidate();
    (void)tick;
    (void)tick_user;
    return 1;
}

static int stream_parser_complete(void *ctx)
{
    struct http_stream *stream = ctx;
    return stream->wire->events_complete(&stream->events);
}

static const struct stream_usage *stream_parser_usage(void *ctx)
{
    struct http_stream *stream = ctx;
    if (!stream->wire->events_usage)
        return NULL;
    return stream->wire->events_usage(&stream->events);
}

/* How long a request may wait on the in-flight snapshot fetch for its wire hint. */
#define WIRE_HINT_FETCH_WAIT_MS 5000

/* The wire `model` speaks: the first matching model_apis rule, else the catalog hint when the
 * preset opted in, else the provider default. NULL means the catalog knows the model needs a
 * protocol hax does not implement; the caller reports it instead of guessing. */
static const struct wire *resolve_model_wire(struct http_provider *provider, const char *model)
{
    for (size_t i = 0; i < provider->n_wire_rules; i++)
        if (fnmatch(provider->wire_rules[i].pattern, model, 0) == 0)
            return provider->wire_rules[i].wire;

    if (provider->catalog_wires && provider->catalog_id) {
        struct catalog_entry entry;
        catalog_lookup(provider->catalog_id, model, &entry);
        if (!entry.api) {
            /* A fresh install may still be fetching the snapshot, and guessing here would
             * speak the wrong protocol to the model: wait, bounded, and look again. A fetch
             * outliving the wait keeps running so later requests can route by it. */
            catalog_wait(WIRE_HINT_FETCH_WAIT_MS);
            catalog_lookup(provider->catalog_id, model, &entry);
        }
        if (entry.api)
            return wire_find(entry.api);
    }
    return provider->wire;
}

/* The member `model`'s reasoning replays under: an explicit reasoning_roundtrip pins one for
 * every model, else the catalog's per-model hint, else the preset default. The result is
 * borrowed from static storage or from the provider, so it outlives the request. */
static const char *resolve_model_reasoning_field(const struct http_provider *provider,
                                                 const char *model)
{
    if (provider->reasoning_field_pinned || !provider->catalog_id)
        return provider->reasoning_field;

    struct catalog_entry entry;
    catalog_lookup(provider->catalog_id, model, &entry);
    return entry.interleaved_field ? entry.interleaved_field : provider->reasoning_field;
}

static int http_provider_stream(struct provider *base, const struct context *context,
                                const char *model, stream_cb callback, void *callback_user,
                                http_tick_cb tick, void *tick_user)
{
    struct http_provider *provider = (struct http_provider *)base;
    const struct wire *wire = resolve_model_wire(provider, model);
    if (!wire) {
        char *message = xasprintf("model %s needs a protocol hax does not support", model);
        struct stream_event error = {.kind = EV_ERROR, .u.error = {.message = message}};
        callback(&error, callback_user);
        free(message);
        return -1;
    }

    struct http_stream stream = {.provider = provider, .wire = wire};
    struct wire_body_opts opts = {
        .extra_body = provider->extra_body,
        .cache_ttl = provider->cache_ttl,
    };

    if (wire == &WIRE_ANTHROPIC_MESSAGES) {
        opts.cache_markers =
            config_scoped_bool_or(provider->config_prefix, "cache", provider->cache_default);
        opts.max_tokens = http_provider_max_tokens(base, model);
        opts.thinking_mode = resolve_thinking_mode(provider, context->effort);
        opts.thinking_budget = config_scoped_int(provider->config_prefix, "thinking_budget");
        opts.show_reasoning = config_bool("show_reasoning");
        opts.allow_empty_signature = provider->allow_empty_signature;
        if (provider->raw_endpoint)
            opts.anthropic_version = provider->version;
    } else {
        /* Cache planning depends on rates populated by the startup metadata probe. Bounded: a
         * router-autoload probe can take minutes, while rate-reporting probes answer quickly,
         * and the request itself waits for the model to load anyway. */
        model_meta_wait_ms(base, MODEL_META_PROBE_WAIT_MS);
        struct catalog_entry rates;
        model_meta_rates(base, model, &rates);
        stream.cache = chat_plan_cache(&rates, provider->cache_mode, provider->cache_ttl);
        opts.cache_markers = stream.cache.send_breakpoints;
        opts.session_cache_key = provider->send_cache_key ? provider->session_id : NULL;
        opts.reasoning_field = resolve_model_reasoning_field(provider, model);
        opts.reasoning_format = provider->reasoning_format;
        opts.emit_progress = provider->emit_progress;
        opts.request_cost = provider->request_cost;
    }

    char *body = wire_build_body(wire, context, provider_stable_id(base), model, &opts);
    if (!body)
        return -1;

    char *endpoint =
        wire == provider->wire ? NULL : xasprintf("%s%s", provider->base_url, wire->path);
    struct stream_retry request = {
        .endpoint = endpoint ? endpoint : provider->endpoint,
        .body = body,
        .body_len = strlen(body),
        .ctx = &stream,
        .build_headers = stream_build_headers,
        .recover = provider->bearer_token ? stream_recover : NULL,
        .parser_init = stream_parser_init,
        .parser_feed = handle_sse_data,
        .parser_finalize = stream_parser_finalize,
        .parser_free = stream_parser_free,
        .parser_complete = stream_parser_complete,
        .parser_usage = stream_parser_usage,
    };
    int result = stream_retry_run(&request, callback, callback_user, tick, tick_user);
    free(endpoint);
    free(body);
    return result;
}

static void http_provider_destroy(struct provider *base)
{
    struct http_provider *provider = (struct http_provider *)base;
    model_meta_release(base);
    for (size_t i = 0; i < provider->n_wire_rules; i++)
        free(provider->wire_rules[i].pattern);
    free(provider->wire_rules);
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
    /* "catalog" declares per-model routing (config_provider wires it up); there is no fixed
     * wire to pick here, so the preset default covers the unmapped models. */
    if (strcasecmp(api, "catalog") == 0)
        return fallback;

    const struct wire *wire = wire_find(api);
    if (!wire || wire == &WIRE_ANTHROPIC_MESSAGES) {
        hax_warn("unknown api %s (expected 'chat' or 'responses') — using default", api);
        return fallback;
    }
    return wire;
}

static enum chat_cache_mode resolve_cache_mode(const char *prefix, int automatic)
{
    /* Different fallbacks distinguish a parsed boolean from auto, unset, or invalid input. */
    int with_false_fallback = config_scoped_bool_or(prefix, "cache", 0);
    int with_true_fallback = config_scoped_bool_or(prefix, "cache", 1);

    if (with_false_fallback == with_true_fallback)
        return with_true_fallback ? CHAT_CACHE_ON : CHAT_CACHE_OFF;
    return automatic ? CHAT_CACHE_AUTO : CHAT_CACHE_OFF;
}

/* `*pinned` reports an explicit setting, including an "off" that must survive a catalog hint.
 * "auto" asks for the default resolution, like the other tri-state settings, rather than naming
 * a member; anything else is a member name. */
static char *resolve_configured_reasoning_field(const char *prefix, const char *preset_default,
                                                int *pinned)
{
    const char *configured = config_scoped_str(prefix, "reasoning_roundtrip");
    if (configured && strcmp(configured, "auto") == 0)
        configured = NULL;

    const char *field = preset_default;
    *pinned = configured != NULL;
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
    char **headers = build_headers(provider, provider->wire, 0);

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
    /* Messages effort levels steer adaptive thinking; only an explicit budget/off pin rules
     * that out — an unconfigured default upgrades per request when an effort is chosen, and
     * an unrecognized value falls back to that default at request time. Only a pure Messages
     * provider hides the ladder: on a mixed one, models routed to other wires still take it. */
    if (provider->wire == &WIRE_ANTHROPIC_MESSAGES && !provider->n_wire_rules &&
        !provider->catalog_wires) {
        const char *mode = config_scoped_str(provider->config_prefix, "thinking_mode");
        if (mode && (strcasecmp(mode, "budget") == 0 || strcasecmp(mode, "off") == 0))
            return 0;
    }
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

const char *http_provider_api_key(const struct provider *provider)
{
    return ((const struct http_provider *)provider)->api_key;
}

char **http_provider_metadata_headers(const struct provider *provider)
{
    const struct http_provider *hp = (const struct http_provider *)provider;
    return build_headers(hp, hp->wire, 0);
}

void http_provider_prepare_base_url_availability(const char *base_url, const char *api_key,
                                                 char *const *extra_headers,
                                                 struct provider_availability *out)
{
    out->available = 0;
    out->reason = xstrdup("server not reachable");
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

/* Parse <prefix>.model_apis — glob patterns mapped to dialect names, first match winning in
 * written order — into owned rules, dropping invalid members with a warning. */
static void resolve_wire_rules(struct http_provider *provider, const char *prefix)
{
    if (!prefix)
        return;
    char *key = xasprintf("%s.model_apis", prefix);
    const json_t *node = config_json_node(key);
    if (node && !json_is_object(node))
        hax_warn("%s must be a JSON object of pattern/dialect members — ignoring it", key);
    if (json_is_object(node)) {
        provider->wire_rules =
            xcalloc(json_object_size((json_t *)node), sizeof(*provider->wire_rules));
        const char *pattern;
        json_t *value;
        json_object_foreach((json_t *)node, pattern, value)
        {
            const struct wire *wire = wire_find(json_string_value(value));
            if (!wire) {
                hax_warn("%s: '%s' needs a dialect value (openai-completions, openai-responses, "
                         "anthropic-messages) — ignoring it",
                         key, pattern);
                continue;
            }
            provider->wire_rules[provider->n_wire_rules].pattern = xstrdup(pattern);
            provider->wire_rules[provider->n_wire_rules].wire = wire;
            provider->n_wire_rules++;
        }
    }
    free(key);
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
    provider->raw_endpoint = preset->raw_endpoint;
    provider->bearer_token = preset->bearer_token;
    /* A raw endpoint (Vertex) names the complete stream URL; wire paths derive per request. */
    provider->endpoint = preset->raw_endpoint
                             ? xstrdup(provider->base_url)
                             : xasprintf("%s%s", provider->base_url, provider->wire->path);
    resolve_wire_rules(provider, preset->config_prefix);
    provider->catalog_wires = preset->catalog_wires;
    /* Resolved regardless of the default wire: per-model rules can route to Messages. */
    const char *version = config_scoped_str(preset->config_prefix, "version");
    const char *fallback = preset->default_version ? preset->default_version
                                                   : MESSAGES_DEFAULT_VERSION;
    provider->version = xstrdup(version && *version ? version : fallback);

    provider->send_cache_key = config_scoped_bool_or(preset->config_prefix, "send_cache_key",
                                                     preset->send_cache_key_default);
    provider->emit_progress = preset->emit_progress;
    provider->request_cost =
        config_scoped_bool_or(preset->config_prefix, "request_cost", preset->request_cost);
    provider->cache_mode = resolve_cache_mode(preset->config_prefix, preset->cache_auto_default);
    provider->cache_ttl = xstrdup(provider_cache_ttl(preset->config_prefix));
    provider->reasoning_field = resolve_configured_reasoning_field(
        preset->config_prefix, preset->reasoning_replay_field, &provider->reasoning_field_pinned);
    provider->reasoning_format = chat_reasoning_format_parse(
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
