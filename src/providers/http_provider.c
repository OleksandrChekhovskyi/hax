/* SPDX-License-Identifier: MIT */
#include "providers/http_provider.h"

#include <fnmatch.h>
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "catalog.h"
#include "config.h"
#include "diag.h"
#include "effort.h"
#include "model_meta.h"
#include "provider.h"
#include "util.h"
#include "providers/anthropic_body.h"
#include "providers/anthropic_models.h"
#include "providers/chat_body.h"
#include "providers/openai_models.h"
#include "providers/provider_config.h"
#include "providers/registry.h"
#include "providers/stream_retry.h"
#include "providers/wire.h"
#include "system/path.h"
#include "transport/http.h"

#define MESSAGES_DEFAULT_VERSION    "2023-06-01"
#define MESSAGES_DEFAULT_MAX_TOKENS 32000

/* The dialect of the provider's model-metadata side — the /models listing and probe, and the
 * auth scheme those requests use. A property of the endpoint, not of the per-model request
 * wire: a mixed-protocol gateway serves one catalog shape however each model is spoken to. */
enum http_metadata_api {
    HTTP_METADATA_OPENAI,    /* flat {"data": [...]} list, Bearer auth */
    HTTP_METADATA_ANTHROPIC, /* cursor-paginated list, x-api-key + version auth */
};

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
    const struct wire *wire;          /* default; wire_rules and catalog hints override per model */
    const struct wire *metadata_wire; /* auth scheme for /models and probe requests */
    struct wire_rule *wire_rules;
    size_t n_wire_rules;
    int catalog_wires;
    char *version; /* anthropic-version; sent on Messages requests */
    int send_cache_key;
    int request_cost;
    enum chat_cache_mode cache_mode;
    char *cache_ttl;
    char *reasoning_field;
    int reasoning_field_pinned; /* configured explicitly; no catalog hint may override it */
    enum chat_reasoning_format reasoning_format;
    enum anthropic_thinking_mode default_thinking_mode;
    int strict_signatures;
    int cache_default; /* Messages cache_control default; chat uses cache_mode */
    char **extra_headers;
    json_t *extra_body;
    struct http_auth_source auth; /* zeroed ops: the api_key authenticates requests */
    /* A probe skipped on stale credentials, relaunched after the next authenticated stream. */
    int probe_deferred;
    char *default_model;
    char *default_effort;

    const char *length_hint;    /* borrowed for the provider lifetime */
    const char *const *efforts; /* borrowed, or aliases owned_efforts */
    const char **owned_efforts; /* owned array of borrowed strings; NULL when not narrowed */
    size_t n_efforts;
    http_parse_model_cb parse_model;
};

static enum anthropic_thinking_mode resolve_thinking_mode(const struct http_provider *provider,
                                                          const char *effort)
{
    /* Messages has no "none" effort value: it means no reasoning, which is thinking off. */
    if (effort && strcmp(effort, "none") == 0)
        return ANTHROPIC_THINKING_OFF;
    const char *configured = config_scoped_str(provider->config_prefix, "thinking_mode");
    if (configured && *configured) {
        int parsed = anthropic_thinking_mode_parse(configured);
        if (parsed >= 0)
            return (enum anthropic_thinking_mode)parsed;
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

/* Owned NULL-terminated headers; free with string_array_free. Credentials come from the auth
 * source when the provider has one, otherwise the auth scheme follows the wire: Bearer for the
 * OpenAI family, x-api-key plus the version header for Messages. Streaming requests add the
 * SSE Accept and the JSON Content-Type. */
static char **build_headers(const struct http_provider *provider, const struct wire *wire,
                            int streaming)
{
    if (provider->auth.ops) {
        char **credentials =
            provider->auth.ops->headers(provider->auth.state, provider->session_id, streaming);
        const char *fixed[3];
        size_t n_fixed = 0;
        if (streaming) {
            fixed[n_fixed++] = "Accept: text/event-stream";
            fixed[n_fixed++] = "Content-Type: application/json";
        }
        fixed[n_fixed] = NULL;
        char **fixed_and_extra =
            string_array_concat(fixed, (const char *const *)provider->extra_headers);
        char **headers = string_array_concat((const char *const *)credentials,
                                             (const char *const *)fixed_and_extra);
        string_array_free(fixed_and_extra);
        string_array_free(credentials);
        return headers;
    }

    char *auth = NULL;
    char *version = NULL;
    if (wire == &WIRE_ANTHROPIC_MESSAGES) {
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
    struct http_provider *provider;
    const struct wire *wire; /* resolved for this request's model */
    struct chat_cache_plan cache;
    union wire_events events;
    int auth_retried; /* per-stream guard for the auth source's unauthorized recovery */
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

static int stream_auth_recover(void *ctx, long http_status, http_tick_cb tick, void *tick_user)
{
    struct http_stream *stream = ctx;
    return http_status == 401 &&
           stream->provider->auth.ops->recover(stream->provider->auth.state, &stream->auth_retried,
                                               tick, tick_user);
}

static char *stream_auth_error_message(void *ctx, long http_status, const char *error_body)
{
    (void)error_body;
    struct http_stream *stream = ctx;
    if (http_status != 401)
        return NULL;
    return stream->provider->auth.ops->unauthorized_message(stream->provider->auth.state);
}

/* How long a request may wait on the in-flight snapshot fetch for its wire hint. */
#define WIRE_HINT_FETCH_WAIT_MS 5000

/* The wire `model` speaks: the first matching model_apis rule, else the catalog hint on a
 * catalog-routed provider, else the provider default. NULL means the catalog knows the model
 * needs a protocol hax does not implement; the caller reports it instead of guessing. */
static const struct wire *resolve_model_wire(struct http_provider *provider, const char *model)
{
    for (size_t i = 0; i < provider->n_wire_rules; i++)
        if (fnmatch(provider->wire_rules[i].pattern, model, 0) == 0)
            return provider->wire_rules[i].wire;

    if (provider->catalog_wires) {
        const char *provider_id = provider_stable_id(&provider->base);
        struct catalog_entry entry;
        catalog_lookup(provider_id, provider->catalog_id, model, &entry);
        if (!entry.api && provider->catalog_id) {
            /* A fresh install may still be fetching the snapshot, and guessing here would
             * speak the wrong protocol to the model: wait, bounded, and look again. A fetch
             * outliving the wait keeps running so later requests can route by it. */
            catalog_wait(WIRE_HINT_FETCH_WAIT_MS);
            catalog_lookup(provider_id, provider->catalog_id, model, &entry);
        }
        if (entry.api)
            return wire_find(entry.api);
    }
    return provider->wire;
}

/* The member `model`'s reasoning replays under: an explicit reasoning_roundtrip pins one for
 * every model, else the catalog's per-model hint, else the def default. The result is
 * borrowed from static storage or from the provider, so it outlives the request. */
static const char *resolve_model_reasoning_field(const struct http_provider *provider,
                                                 const char *model)
{
    if (provider->reasoning_field_pinned)
        return provider->reasoning_field;

    struct catalog_entry entry;
    catalog_lookup(provider_stable_id(&provider->base), provider->catalog_id, model, &entry);
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

    if (provider->auth.ops) {
        if (provider->auth.ops->prepare(provider->auth.state, 1, tick, tick_user) != 0) {
            char *message = provider->auth.ops->unauthorized_message(provider->auth.state);
            struct stream_event error = {
                .kind = EV_ERROR,
                .u.error = {.message = message, .http_status = 401},
            };
            callback(&error, callback_user);
            free(message);
            return -1;
        }
        if (provider->probe_deferred) {
            /* Non-blocking: relaunches the background metadata probe now that prepare had its
             * refresh opportunity. Still-stale credentials just defer it again. */
            provider->probe_deferred = 0;
            model_meta_refresh(base, model);
        }
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
        opts.allow_empty_signature = !provider->strict_signatures;
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
        .parser_init = stream_parser_init,
        .parser_feed = handle_sse_data,
        .parser_finalize = stream_parser_finalize,
        .parser_free = stream_parser_free,
        .parser_complete = stream_parser_complete,
        .parser_usage = stream_parser_usage,
    };
    if (provider->auth.ops) {
        request.recover = stream_auth_recover;
        request.error_message = stream_auth_error_message;
    }
    int result = stream_retry_run(&request, callback, callback_user, tick, tick_user);
    free(endpoint);
    free(body);
    return result;
}

static void http_provider_destroy(struct provider *base)
{
    struct http_provider *provider = (struct http_provider *)base;
    model_meta_release(base);
    if (provider->auth.ops)
        provider->auth.ops->destroy(provider->auth.state);
    free(provider->default_model);
    free(provider->default_effort);
    for (size_t i = 0; i < provider->n_wire_rules; i++)
        free(provider->wire_rules[i].pattern);
    free(provider->wire_rules);
    free(provider->owned_efforts);
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

/* The def's api — a pinned def's own, else config overlaid on the def — names the default
 * request protocol; "catalog" declares per-model routing with Chat Completions covering the
 * models the catalog leaves unmapped. `*api_out` receives the resolved name. NULL after
 * reporting an unsupported value. */
static const struct wire *resolve_wire(const struct provider_def *def, const char *prefix,
                                       const char **api_out)
{
    const char *configured = config_scoped_str(prefix, "api");
    if (configured && !*configured)
        configured = NULL;
    const char *api = def->api ? def->api : "openai-completions";
    if (def->pinned) {
        /* Silently accepting the value would fake a switch the pin just refused. */
        if (configured)
            hax_warn("provider '%s': api is pinned to %s — use model_apis for per-model "
                     "protocols, or a custom provider",
                     def->id, api);
    } else if (configured) {
        api = configured;
    }
    *api_out = api;
    if (strcasecmp(api, "catalog") == 0)
        return &WIRE_OPENAI_CHAT;
    const struct wire *wire = wire_find(api);
    if (!wire)
        hax_err("provider '%s': unsupported api '%s' "
                "(supported: openai-completions, openai-responses, anthropic-messages, catalog)",
                def->id, api);
    return wire;
}

static enum chat_cache_mode resolve_cache_mode(const char *prefix, const char *def_default)
{
    /* Different fallbacks distinguish a parsed boolean from auto, unset, or invalid input. */
    int with_false_fallback = config_scoped_bool_or(prefix, "cache", 0);
    int with_true_fallback = config_scoped_bool_or(prefix, "cache", 1);

    if (with_false_fallback == with_true_fallback)
        return with_true_fallback ? CHAT_CACHE_ON : CHAT_CACHE_OFF;
    if (def_default && strcasecmp(def_default, "auto") == 0)
        return CHAT_CACHE_AUTO;
    if (def_default && strcasecmp(def_default, "on") == 0)
        return CHAT_CACHE_ON;
    return CHAT_CACHE_OFF;
}

/* `*pinned` reports an explicit setting, including an "off" that must survive a catalog hint.
 * "auto" asks for the default resolution, like the other tri-state settings, rather than naming
 * a member; anything else is a member name. */
static char *resolve_configured_reasoning_field(const char *prefix, const char *def_default,
                                                int *pinned)
{
    const char *configured = config_scoped_str(prefix, "reasoning_roundtrip");
    if (configured && strcmp(configured, "auto") == 0)
        configured = NULL;

    const char *field = def_default;
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
    return build_headers(hp, hp->metadata_wire, 0);
}

http_parse_model_cb http_provider_parse_model(const struct provider *provider)
{
    return ((const struct http_provider *)provider)->parse_model;
}

const struct http_auth_source *http_provider_auth(const struct provider *provider)
{
    return &((const struct http_provider *)provider)->auth;
}

void http_provider_defer_probe(struct provider *provider)
{
    ((struct http_provider *)provider)->probe_deferred = 1;
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

/* Expand a "{port}" placeholder in a def's default base_url: providers.<name>.port, else the
 * def's own port. Returns the owned expansion, or NULL when the URL carries no placeholder or
 * no port resolves. */
static char *expand_port_template(const struct provider_def *def)
{
    const char *url = def->base_url;
    const char *placeholder = url ? strstr(url, "{port}") : NULL;
    if (!placeholder)
        return NULL;
    /* The typed read parses and bounds-checks a registered setting (llamacpp), falling back to
     * its registered default; the range guard covers unregistered ports, so a malformed value
     * degrades to the def's default instead of a malformed URL. */
    char *key = xasprintf("providers.%s.port", def->id);
    int port = config_int(key);
    free(key);
    if (port < 1 || port > 65535)
        port = def->port;
    if (port < 1)
        return NULL;
    return xasprintf("%.*s%d%s", (int)(placeholder - url), url, port,
                     placeholder + strlen("{port}"));
}

/* Resolve the def's endpoint: a pinned def's own base_url; otherwise the configured
 * providers.<id>.base_url verbatim, else the def's default with any "{port}" placeholder
 * expanded. Owned; NULL when nothing resolves. The trailing slash is trimmed so
 * "<base>/models" and "<base><wire path>" never double it. */
static char *def_base_url(const struct provider_def *def)
{
    const char *base = def->base_url;
    char *expanded = NULL;
    if (!def->pinned) {
        char *key = xasprintf("providers.%s.base_url", def->id);
        const char *configured = config_str_nonempty(key);
        free(key);
        if (configured) {
            base = configured;
        } else {
            expanded = expand_port_template(def);
            if (expanded)
                base = expanded;
        }
    }
    char *trimmed = base ? dup_trim_trailing_slash(base) : NULL;
    free(expanded);
    return trimmed;
}

void http_provider_availability(const struct provider_def *def, struct provider_availability *out)
{
    char *base = def_base_url(def);
    if (!base) {
        out->available = 0;
        out->reason = xstrdup(def->unconfigured_reason ? def->unconfigured_reason : "no base_url");
        return;
    }

    char *prefix = xasprintf("providers.%s", def->id);
    const char *inline_key = config_scoped_str(prefix, "api_key");
    const char *key_env = def->pinned ? def->api_key_env : config_scoped_str(prefix, "api_key_env");
    if (!def->pinned && (!key_env || !*key_env))
        key_env = def->api_key_env;
    if ((inline_key && *inline_key) || key_env) {
        const char *api_key = provider_api_key(prefix, key_env);
        out->available = api_key != NULL;
        if (!out->available) {
            /* Name the exact variable or key to set. */
            out->reason = key_env ? xasprintf("%s not set", key_env)
                                  : xasprintf("providers.%s.api_key not set", def->id);
        }
    } else if (!def->probe) {
        /* Keyless without a probing def: configuration is the whole check, because a generic
         * endpoint may serve only its completion route and no /models. */
        out->available = 1;
    } else {
        char **extra_headers = provider_extra_headers(prefix);
        http_provider_prepare_base_url_availability(base, NULL, extra_headers, out);
        string_array_free(extra_headers);
    }
    free(prefix);
    free(base);
}

/* A declared <prefix>.model_apis block, valid members or not: routing intent makes the provider
 * a mixed-protocol gateway even when every member is invalid. */
static int wire_rules_declared(const char *prefix)
{
    char *key = xasprintf("%s.model_apis", prefix);
    const json_t *node = config_json_node(key);
    free(key);
    return json_is_object(node) && json_object_size((json_t *)node) > 0;
}

/* Parse <prefix>.model_apis — glob patterns mapped to dialect names, first match winning in
 * written order — into owned rules, dropping invalid members with a warning. */
static void resolve_wire_rules(struct http_provider *provider, const char *prefix)
{
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

static char *resolve_catalog_id(const struct provider_def *def, const char *prefix)
{
    char *key = xasprintf("%s.catalog_id", prefix);
    const char *configured = config_str(key);
    free(key);
    /* An explicit value wins, including an empty opt-out; only silence takes the def's. */
    const char *catalog_id = configured ? (*configured ? configured : NULL) : def->catalog_id;
    return catalog_id ? xstrdup(catalog_id) : NULL;
}

/* "openai" or "anthropic" to the enum; -1 for anything else (unset, "auto", a typo). */
static int metadata_api_parse(const char *value)
{
    if (value && strcasecmp(value, "openai") == 0)
        return HTTP_METADATA_OPENAI;
    if (value && strcasecmp(value, "anthropic") == 0)
        return HTTP_METADATA_ANTHROPIC;
    return -1;
}

/* The dialect the def's own metadata hooks were written against: its declared metadata_api,
 * else its default wire's family. */
static enum http_metadata_api def_metadata_api(const struct provider_def *def,
                                               const struct wire *wire)
{
    int declared = metadata_api_parse(def->metadata_api);
    if (declared >= 0)
        return (enum http_metadata_api)declared;
    return wire == &WIRE_ANTHROPIC_MESSAGES ? HTTP_METADATA_ANTHROPIC : HTTP_METADATA_OPENAI;
}

/* An unrecognized value falls back like the other tri-state settings rather than failing
 * construction. */
static enum http_metadata_api resolve_metadata_api(const struct provider_def *def,
                                                   const struct wire *wire, const char *prefix)
{
    const char *configured = config_scoped_str(prefix, "metadata_api");
    int parsed = metadata_api_parse(configured);
    if (parsed >= 0)
        return (enum http_metadata_api)parsed;
    if (configured && *configured && strcasecmp(configured, "auto") != 0)
        hax_warn("unknown metadata_api '%s' (openai or anthropic) — using default", configured);
    return def_metadata_api(def, wire);
}

/* A pure Messages provider offers only the efforts its dialect can express. A mixed provider
 * keeps the full ladder for models routed to other wires; per-model metadata narrows the rest. */
static void narrow_messages_efforts(struct http_provider *provider)
{
    if (provider->wire != &WIRE_ANTHROPIC_MESSAGES || provider->n_wire_rules > 0 ||
        provider->catalog_wires || provider->n_efforts == 0)
        return;

    provider->owned_efforts = xcalloc(provider->n_efforts, sizeof(*provider->owned_efforts));
    size_t n_kept = 0;
    for (size_t i = 0; i < provider->n_efforts; i++) {
        /* "none" stays offered: requests express it as thinking off. */
        if (anthropic_effort_expressible(provider->efforts[i]) ||
            strcmp(provider->efforts[i], "none") == 0)
            provider->owned_efforts[n_kept++] = provider->efforts[i];
    }
    provider->efforts = provider->owned_efforts;
    provider->n_efforts = n_kept;
}

static const char *resolve_display_name(const struct provider_def *def, const char *prefix)
{
    const char *configured = config_scoped_str(prefix, "display_name");
    if (configured && *configured)
        return configured;
    return def->display_name ? def->display_name : def->id;
}

struct provider *http_provider_new(const struct provider_def *def)
{
    const char *name = def->id;
    char *prefix = xasprintf("providers.%s", name);
    const char *api = NULL;
    const struct wire *wire = resolve_wire(def, prefix, &api);
    if (!wire) {
        free(prefix);
        return NULL;
    }
    enum http_metadata_api metadata_api = resolve_metadata_api(def, wire, prefix);
    /* model_apis or api "catalog" declares a mixed-protocol gateway: catalog hints route models,
     * and every dialect's fields are live for the warn pass below. */
    int routes_wires = strcasecmp(api, "catalog") == 0 || wire_rules_declared(prefix);

    /* The Anthropic metadata dialect consumes `version` for its /models headers even when no
     * request wire would. */
    static const char *const METADATA_FIELDS[] = {"version", NULL};
    /* A pinned def's api is fixed (resolve_wire warns when config sets it); an unpinned def's
     * wire already reflects config. Key fields are consumed only when a static key
     * authenticates the provider; the port only when the def's base_url is port-templated. */
    unsigned classes = wire == &WIRE_ANTHROPIC_MESSAGES ? PROVIDER_FIELD_ANTHROPIC
                       : wire == &WIRE_OPENAI_RESPONSES ? PROVIDER_FIELD_OPENAI_RESPONSES
                                                        : PROVIDER_FIELD_OPENAI_CHAT;
    if (routes_wires)
        classes |= PROVIDER_FIELD_OPENAI | PROVIDER_FIELD_ANTHROPIC;
    if (!def->auth_source)
        classes |= PROVIDER_FIELD_KEYED | (def->pinned ? 0 : PROVIDER_FIELD_UNPINNED);
    if (def->base_url && strstr(def->base_url, "{port}"))
        classes |= PROVIDER_FIELD_PORT_TEMPLATED;
    provider_warn_unused_fields(name, wire->id, classes,
                                metadata_api == HTTP_METADATA_ANTHROPIC ? METADATA_FIELDS : NULL);

    const char *configured_base = config_scoped_str(prefix, "base_url");
    /* Silently accepting the key would fake a redirect the pin just refused. */
    if (def->pinned && configured_base && *configured_base)
        hax_warn("provider '%s': base_url is pinned to %s — use a custom provider for "
                 "another endpoint",
                 name, def->base_url);
    char *base_url = def_base_url(def);
    if (!base_url) {
        char *key = xasprintf("%s.base_url", prefix);
        const struct config_setting *setting = config_setting_find(key);
        if (setting && setting->env_var)
            hax_err("provider '%s': no base_url (set %s, or %s in config.json)", name,
                    setting->env_var, key);
        else
            hax_err("provider '%s': no base_url (set %s in config.json)", name, key);
        free(key);
        free(prefix);
        return NULL;
    }

    int model_discovered = 0;
    if (def->discover && def->discover(base_url, &model_discovered) != 0) {
        free(base_url);
        free(prefix);
        return NULL;
    }

    /* Credentials must exist before anything else is built: a logged-out provider fails
     * construction with the hook's diagnostics, like a missing base_url. */
    struct http_auth_source auth = {0};
    if (def->auth_source && def->auth_source(def, &auth) != 0) {
        free(base_url);
        free(prefix);
        return NULL;
    }

    struct http_provider *provider = xcalloc(1, sizeof(*provider));
    provider->base_url = base_url;
    provider->config_prefix = prefix;
    provider->auth = auth;
    if (!provider->auth.ops) {
        const char *api_key_env =
            def->pinned ? def->api_key_env : config_scoped_str(prefix, "api_key_env");
        if (!def->pinned && (!api_key_env || !*api_key_env))
            api_key_env = def->api_key_env;
        const char *api_key = provider_api_key(prefix, api_key_env);
        provider->api_key = api_key ? xstrdup(api_key) : NULL;
    }
    provider->name = xstrdup(resolve_display_name(def, prefix));
    provider->catalog_id = resolve_catalog_id(def, prefix);
    provider->wire = wire;
    provider->endpoint = xasprintf("%s%s", provider->base_url, provider->wire->path);
    /* On the OpenAI side any OpenAI-family wire carries the same Bearer scheme; only a Messages
     * default wire paired with OpenAI-shaped metadata needs the explicit Chat stand-in. */
    if (metadata_api == HTTP_METADATA_ANTHROPIC)
        provider->metadata_wire = &WIRE_ANTHROPIC_MESSAGES;
    else if (provider->wire == &WIRE_ANTHROPIC_MESSAGES)
        provider->metadata_wire = &WIRE_OPENAI_CHAT;
    else
        provider->metadata_wire = provider->wire;
    resolve_wire_rules(provider, prefix);
    provider->catalog_wires = routes_wires;
    /* Catalog routing with no rules, no catalog identity, and no configured api hint would
     * silently send every model to the default wire. */
    if (provider->catalog_wires && !provider->catalog_id && provider->n_wire_rules == 0 &&
        !catalog_config_routes_models(name))
        hax_warn("provider '%s': models route by catalog metadata, but catalog_id is empty",
                 provider->name);
    /* Resolved regardless of the default wire: per-model rules can route to Messages. */
    const char *version = config_scoped_str(prefix, "version");
    provider->version = xstrdup(version && *version ? version : MESSAGES_DEFAULT_VERSION);

    provider->send_cache_key = config_scoped_bool_or(prefix, "send_cache_key", def->send_cache_key);
    provider->request_cost = config_scoped_bool_or(prefix, "request_cost", def->request_cost);
    provider->cache_mode = resolve_cache_mode(prefix, def->cache);
    provider->cache_ttl = xstrdup(provider_cache_ttl(prefix));
    provider->reasoning_field = resolve_configured_reasoning_field(
        prefix, def->reasoning_roundtrip, &provider->reasoning_field_pinned);
    provider->reasoning_format = chat_reasoning_format_parse(
        config_scoped_str(prefix, "reasoning_format"),
        chat_reasoning_format_parse(def->reasoning_format, CHAT_REASONING_FLAT));
    provider->cache_default = def->cache && strcasecmp(def->cache, "on") == 0;
    int thinking_mode = anthropic_thinking_mode_parse(def->thinking_mode);
    provider->default_thinking_mode = thinking_mode >= 0
                                          ? (enum anthropic_thinking_mode)thinking_mode
                                          : ANTHROPIC_THINKING_BUDGET;
    provider->strict_signatures = def->strict_signatures;
    /* Def-declared headers first, then config-declared ones; both reach every request. */
    char **static_headers = def->static_headers ? def->static_headers() : NULL;
    char **config_headers = provider_extra_headers(prefix);
    provider->extra_headers = string_array_concat((const char *const *)static_headers,
                                                  (const char *const *)config_headers);
    string_array_free(config_headers);
    string_array_free(static_headers);
    provider->extra_body = provider_extra_body(prefix);
    if (def->extra_body) {
        /* The user's members merge over the def's, so config can override endpoint defaults. */
        json_t *merged = json_loads(def->extra_body, 0, NULL);
        if (merged) {
            if (provider->extra_body)
                json_object_update_recursive(merged, provider->extra_body);
            json_decref(provider->extra_body);
            provider->extra_body = merged;
        }
    }

    provider->length_hint = def->length_hint;
    /* Efforts are advisory offers narrowed by per-model metadata, so they default on; defs opt
     * out backends with no categorical effort. */
    if (!def->no_efforts) {
        provider->efforts = EFFORT_LADDER;
        provider->n_efforts = EFFORT_LADDER_N;
    }
    narrow_messages_efforts(provider);
    provider->parse_model = def->parse_model;

    char session_id[37];
    gen_uuid_v4(session_id);
    provider->session_id = xstrdup(session_id);

    if (def->load_defaults)
        def->load_defaults(&provider->default_model, &provider->default_effort);
    provider->base.default_model = provider->default_model;
    provider->base.default_effort = provider->default_effort;

    provider->base.id = name;
    provider->base.model_discovered = model_discovered;
    provider->base.name = provider->name;
    provider->base.catalog_id = provider->catalog_id;
    provider->base.stream = http_provider_stream;
    if (metadata_api == HTTP_METADATA_ANTHROPIC) {
        provider->base.list_models = anthropic_list_models;
        provider->base.probe_model = anthropic_probe_model;
    } else {
        provider->base.list_models = openai_list_models;
    }
    /* Like parse_model (which only the def's own listing consults), the probe and listing hooks
     * refine the def's metadata dialect: a configured metadata_api that moves the provider to
     * another dialect must keep that dialect's requests, not a mixed pairing. */
    if (metadata_api == def_metadata_api(def, wire)) {
        if (def->probe_model)
            provider->base.probe_model = def->probe_model;
        if (def->list_models)
            provider->base.list_models = def->list_models;
    }
    if (def->query_usage)
        provider->base.query_usage = def->query_usage;
    if (def->model_label)
        provider->base.model_label = def->model_label;
    provider->base.list_efforts = http_provider_list_efforts;
    provider->base.destroy = http_provider_destroy;
    return &provider->base;
}
