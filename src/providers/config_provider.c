/* SPDX-License-Identifier: MIT */
#include "providers/config_provider.h"

#include <ctype.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "effort.h"
#include "model_meta.h"
#include "provider.h"
#include "trace.h"
#include "util.h"
#include "providers/chat_body.h"
#include "providers/http_provider.h"
#include "providers/registry.h"
#include "providers/wire.h"

#define FIELD_ANY (PROVIDER_FIELD_OPENAI | PROVIDER_FIELD_ANTHROPIC)
/* Responses fixes its reasoning shape and round-trip on the wire and never sends explicit cache
 * markers, so those knobs apply to Chat Completions only. */
#define FIELD_CACHE_MARKERS (PROVIDER_FIELD_OPENAI_CHAT | PROVIDER_FIELD_ANTHROPIC)

// clang-format off
static const struct provider_field PROVIDER_FIELDS[] = {
    /* `api` is resolved for every def — pinned ones warn about it in resolve_wire rather than
     * here, so the message can say the value is pinned instead of merely unused. */
    {.leaf = "api",                 .classes = FIELD_ANY},
    {.leaf = "base_url",            .classes = FIELD_ANY},
    {.leaf = "port",                .classes = PROVIDER_FIELD_PORT_TEMPLATED},
    {.leaf = "api_key",             .classes = PROVIDER_FIELD_KEYED, .secret = 1},
    {.leaf = "api_key_env",         .classes = PROVIDER_FIELD_UNPINNED},
    {.leaf = "display_name",        .classes = FIELD_ANY},
    {.leaf = "catalog_id",          .classes = FIELD_ANY},
    {.leaf = "sort_models",         .classes = FIELD_ANY},
    {.leaf = "metadata_api",        .classes = FIELD_ANY},
    {.leaf = "model_apis",          .classes = FIELD_ANY},
    {.leaf = "cache",               .classes = FIELD_CACHE_MARKERS},
    {.leaf = "cache_ttl",           .classes = FIELD_CACHE_MARKERS},
    {.leaf = "send_cache_key",      .classes = PROVIDER_FIELD_OPENAI},
    {.leaf = "request_cost",        .classes = PROVIDER_FIELD_OPENAI_CHAT},
    {.leaf = "reasoning_format",    .classes = PROVIDER_FIELD_OPENAI_CHAT},
    {.leaf = "reasoning_roundtrip", .classes = PROVIDER_FIELD_OPENAI_CHAT},
    {.leaf = "max_tokens",          .classes = PROVIDER_FIELD_ANTHROPIC},
    {.leaf = "thinking_mode",       .classes = PROVIDER_FIELD_ANTHROPIC},
    {.leaf = "thinking_budget",     .classes = PROVIDER_FIELD_ANTHROPIC},
    {.leaf = "version",             .classes = PROVIDER_FIELD_ANTHROPIC},
    {.leaf = "extra_body",          .classes = FIELD_ANY},
    {.leaf = "extra_headers",       .classes = FIELD_ANY, .secret = 1},
};
// clang-format on
#define N_PROVIDER_FIELDS (sizeof(PROVIDER_FIELDS) / sizeof(PROVIDER_FIELDS[0]))

const struct provider_field *provider_fields(size_t *n)
{
    *n = N_PROVIDER_FIELDS;
    return PROVIDER_FIELDS;
}

static unsigned provider_wire_class(const struct wire *wire)
{
    if (wire == &WIRE_ANTHROPIC_MESSAGES)
        return PROVIDER_FIELD_ANTHROPIC;
    if (wire == &WIRE_OPENAI_RESPONSES)
        return PROVIDER_FIELD_OPENAI_RESPONSES;
    return PROVIDER_FIELD_OPENAI_CHAT;
}

static const struct provider_field *field_find(const char *leaf)
{
    for (size_t i = 0; i < N_PROVIDER_FIELDS; i++)
        if (strcmp(PROVIDER_FIELDS[i].leaf, leaf) == 0)
            return &PROVIDER_FIELDS[i];
    return NULL;
}

static int string_list_has(const char *const *list, const char *value)
{
    for (const char *const *entry = list; entry && *entry; entry++)
        if (strcmp(*entry, value) == 0)
            return 1;
    return 0;
}

/* A value beginning with '$' names an environment variable holding the real value, so a
 * secret can stay out of the config file; "$$" escapes a literal leading '$'. Returns the
 * borrowed resolved value, or NULL when the variable is unset or empty. Resolved values are
 * registered with the trace so they never appear in a HAX_TRACE dump. */
static const char *resolve_env_escape(const char *value)
{
    if (value[0] != '$')
        return value;
    if (value[1] == '$')
        return value + 1;
    const char *resolved = getenv(value + 1);
    if (!resolved || !*resolved)
        return NULL;
    trace_register_secret(resolved);
    return resolved;
}

const char *provider_api_key(const char *config_prefix, const char *api_key_env)
{
    const char *key = config_scoped_str(config_prefix, "api_key");
    if (key && *key)
        key = resolve_env_escape(key);
    if (!key || !*key)
        key = api_key_env ? getenv(api_key_env) : NULL;
    if (!key || !*key)
        return NULL;
    trace_register_secret(key);
    return key;
}

const char *provider_cache_ttl(const char *config_prefix)
{
    const char *value = config_scoped_str(config_prefix, "cache_ttl");
    if (!value || !*value)
        return "1h";
    if (strcasecmp(value, "5m") == 0)
        return "5m";
    if (strcasecmp(value, "1h") == 0)
        return "1h";
    hax_warn("unknown cache_ttl '%s' (5m or 1h) — using 1h", value);
    return "1h";
}

/* Members whose value the request machinery owns: overriding them would desynchronize the
 * parser, the tool loop, or the conversation itself, not just tweak the request. `system` and
 * `instructions` carry the system prompt, which the transcript must keep reflecting (replace
 * it with the system_prompt setting instead); `include` carries the encrypted-reasoning entry
 * Responses continuation needs; `n` is here because the stream parser reads choices[0] only —
 * extra completions would be paid for and silently discarded. */
static const char *const EXTRA_BODY_RESERVED[] = {
    "model", "stream", "messages", "input",          "include",
    "n",     "system", "tools",    "stream_options", "instructions",
};

json_t *provider_extra_body(const char *config_prefix)
{
    if (!config_prefix)
        return NULL;
    char *key = xasprintf("%s.extra_body", config_prefix);
    const json_t *node = config_json_node(key);
    json_t *extra_body = NULL;
    if (json_is_object(node)) {
        extra_body = json_deep_copy(node);
        for (size_t i = 0; i < sizeof(EXTRA_BODY_RESERVED) / sizeof(*EXTRA_BODY_RESERVED); i++) {
            if (json_object_get(extra_body, EXTRA_BODY_RESERVED[i])) {
                hax_warn("%s: '%s' is protocol-owned — ignoring it", key, EXTRA_BODY_RESERVED[i]);
                json_object_del(extra_body, EXTRA_BODY_RESERVED[i]);
            }
        }
    } else if (node) {
        hax_warn("%s must be a JSON object — ignoring it", key);
    }
    free(key);
    return extra_body;
}

static void merge_object_recursive(json_t *target, const json_t *extra)
{
    const char *member_name;
    json_t *member;
    json_object_foreach((json_t *)extra, member_name, member)
    {
        json_t *existing = json_object_get(target, member_name);
        if (json_is_object(existing) && json_is_object(member))
            merge_object_recursive(existing, member);
        else
            json_object_set(target, member_name, member);
    }
}

void provider_extra_body_apply(json_t *body, const json_t *extra_body)
{
    if (extra_body)
        merge_object_recursive(body, extra_body);
}

/* RFC 7230 field names are tokens; a separator would smuggle in a second header or make curl
 * fail the whole request rather than this one header. */
static int header_name_valid(const char *name)
{
    if (!*name)
        return 0;
    for (const char *byte = name; *byte; byte++) {
        if (!isalnum((unsigned char)*byte) && !strchr("!#$%&'*+-.^_`|~", *byte))
            return 0;
    }
    return 1;
}

/* Field values are visible characters plus space and tab: no CR/LF/DEL or other controls. */
static int header_value_valid(const char *value)
{
    for (const char *byte = value; *byte; byte++) {
        if (((unsigned char)*byte < ' ' && *byte != '\t') || *byte == 0x7f)
            return 0;
    }
    return 1;
}

char **provider_extra_headers(const char *config_prefix)
{
    if (!config_prefix)
        return NULL;
    char *key = xasprintf("%s.extra_headers", config_prefix);
    const json_t *node = config_json_node(key);
    if (node && !json_is_object(node))
        hax_warn("%s must be a JSON object of name/value members — ignoring it", key);
    if (!json_is_object(node)) {
        free(key);
        return NULL;
    }

    char **headers = xcalloc(json_object_size((json_t *)node) + 1, sizeof(*headers));
    size_t n_headers = 0;
    const char *name;
    json_t *value;
    json_object_foreach((json_t *)node, name, value)
    {
        const char *text = json_string_value(value);
        const char *resolved = text ? resolve_env_escape(text) : NULL;
        /* Validate the resolved value, not the written one: an environment variable holding
         * a newline must not smuggle in a second header. */
        if (!header_name_valid(name))
            hax_warn("%s: invalid header name '%s' — ignoring it", key, name);
        else if (!text)
            hax_warn("%s: header '%s' needs a string value — ignoring it", key, name);
        else if (!resolved)
            hax_warn("%s: header '%s' dropped — %s is not set", key, name, text + 1);
        else if (!*resolved)
            /* curl reads "Name:" with an empty value as suppression, not an empty header. */
            hax_warn("%s: header '%s' needs a non-empty value — ignoring it", key, name);
        else if (!header_value_valid(resolved))
            hax_warn("%s: header '%s' needs a control-character-free value — ignoring it", key,
                     name);
        else
            headers[n_headers++] = xasprintf("%s: %s", name, resolved);
    }
    free(key);
    if (n_headers == 0) {
        free(headers);
        return NULL;
    }
    return headers;
}

/* A leaf outside the shared inventory is still consumed when it is a registered per-provider
 * setting — a compiled-in module knob such as providers.llamacpp.port. Inventory fields are
 * deliberately not rescued this way: a registered field the dialect ignores must keep warning. */
static int module_key_registered(const char *name, const char *leaf)
{
    char *key = xasprintf("providers.%s.%s", name, leaf);
    int registered = config_setting_find(key) != NULL;
    free(key);
    return registered;
}

static const char *cfg(const char *name, const char *leaf);

/* A model_apis block or api "catalog" — configured or from the def — makes the provider a
 * mixed-protocol gateway: models may land on any wire, so every dialect's fields are live. */
static int provider_routes_wires(const char *name)
{
    char *key = xasprintf("providers.%s.model_apis", name);
    const json_t *rules = config_json_node(key);
    free(key);
    if (json_is_object(rules) && json_object_size((json_t *)rules) > 0)
        return 1;
    const struct provider_def *def = provider_find(name);
    const char *api = def && def->pinned ? def->api : cfg(name, "api");
    if (!api && def)
        api = def->api;
    return api && strcasecmp(api, "catalog") == 0;
}

/* A silently ignored member hides a typo or a dialect mix-up. */
void provider_warn_unused_fields(const char *name, const struct wire *wire, unsigned extra_classes,
                                 const char *const *extra)
{
    const char *api_label = wire ? wire->id : name;
    unsigned classes = (wire ? provider_wire_class(wire) : 0) | extra_classes;
    if (provider_routes_wires(name))
        classes |= FIELD_ANY;
    char *key = xasprintf("providers.%s", name);
    char **members = NULL;
    size_t n_members = config_object_keys(key, &members);
    free(key);
    for (size_t i = 0; i < n_members; i++) {
        const struct provider_field *field = field_find(members[i]);
        if (string_list_has(extra, members[i])) {
            ; /* consumed by this provider */
        } else if (field) {
            if (field->classes & classes) {
                ; /* consumed by this provider */
            } else if (field->classes & FIELD_ANY) {
                /* The dialect wording wins whenever some wire does consume the field. */
                hax_warn("provider '%s': field '%s' is not used by %s providers", name, members[i],
                         api_label);
            } else {
                hax_warn("provider '%s': field '%s' is not configurable for this provider", name,
                         members[i]);
            }
        } else if (!module_key_registered(name, members[i])) {
            hax_warn("provider '%s': unknown field '%s' (see docs/providers.md)", name, members[i]);
        }
        free(members[i]);
    }
    free(members);
}

/* Resolve providers.<name>.<leaf> from config (override → file/state; the named lane has no
 * env tier), empty counting as unset. Returns a borrowed pointer into the config tier
 * (process-lifetime) or NULL. */
static const char *cfg(const char *name, const char *leaf)
{
    char *key = xasprintf("providers.%s.%s", name, leaf);
    const char *v = config_str_nonempty(key);
    free(key);
    return v;
}

/* Config value, else def field, else NULL; never empty. */
static const char *resolve(const char *name, const char *leaf, const char *fallback)
{
    const char *v = cfg(name, leaf);
    return v ? v : fallback;
}

/* Expand a "{port}" placeholder in a def's default base_url: providers.<name>.port, else the
 * def's own port. Returns the owned expansion, or NULL when the URL carries no placeholder or
 * no port resolves. */
static char *expand_base_url(const struct provider_def *def)
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

static enum http_metadata_api metadata_api_from_def(const struct provider_def *def)
{
    if (def->metadata_api && strcasecmp(def->metadata_api, "openai") == 0)
        return HTTP_METADATA_OPENAI;
    if (def->metadata_api && strcasecmp(def->metadata_api, "anthropic") == 0)
        return HTTP_METADATA_ANTHROPIC;
    return HTTP_METADATA_BY_WIRE;
}

/* The dialect the def's own metadata hooks were written against. */
static enum http_metadata_api def_metadata_api(const struct provider_def *def,
                                               const struct wire *wire)
{
    enum http_metadata_api api = metadata_api_from_def(def);
    if (api == HTTP_METADATA_BY_WIRE) {
        api = wire == &WIRE_ANTHROPIC_MESSAGES ? HTTP_METADATA_ANTHROPIC : HTTP_METADATA_OPENAI;
    }
    return api;
}

/* Build the provider for `def`. The api field — config (unless pinned), else def, else
 * openai-completions — picks the wire; every other field resolves as config overlaid on the
 * def, with dialect-specific keys read from the same subtree via config_prefix. */
struct provider *provider_def_construct(const struct provider_def *def)
{
    const char *name = def->id;
    const char *api = def->pinned ? def->api : resolve(name, "api", def->api);
    if (!api)
        api = "openai-completions";
    const struct wire *wire;
    if (strcasecmp(api, "catalog") == 0) {
        /* Per-model routing from catalog hints; models the catalog leaves unmapped use Chat
         * Completions, like the shipped gateway defs. */
        wire = &WIRE_OPENAI_CHAT;
    } else {
        wire = wire_find(api);
    }
    if (!wire) {
        hax_err("provider '%s': unsupported api '%s' "
                "(supported: openai-completions, openai-responses, anthropic-messages, catalog)",
                name, api);
        return NULL;
    }
    /* The Anthropic metadata dialect consumes `version` for its /models headers even when no
     * request wire would. */
    static const char *const METADATA_FIELDS[] = {"version", NULL};
    enum http_metadata_api metadata_api = def_metadata_api(def, wire);
    const char *configured_metadata = cfg(name, "metadata_api");
    if (configured_metadata && strcasecmp(configured_metadata, "openai") == 0)
        metadata_api = HTTP_METADATA_OPENAI;
    else if (configured_metadata && strcasecmp(configured_metadata, "anthropic") == 0)
        metadata_api = HTTP_METADATA_ANTHROPIC;

    /* A pinned def's api is fixed (http_provider warns when config sets it); an unpinned
     * def's wire already reflects config. Key fields are consumed only when a static key
     * authenticates the provider; the port only when the def's base_url is port-templated. */
    unsigned extra_classes = 0;
    if (!def->auth_source)
        extra_classes = PROVIDER_FIELD_KEYED | (def->pinned ? 0 : PROVIDER_FIELD_UNPINNED);
    if (def->base_url && strstr(def->base_url, "{port}"))
        extra_classes |= PROVIDER_FIELD_PORT_TEMPLATED;
    provider_warn_unused_fields(name, wire, extra_classes,
                                metadata_api == HTTP_METADATA_ANTHROPIC ? METADATA_FIELDS : NULL);

    char *computed_base = def->pinned ? NULL : expand_base_url(def);
    const char *base =
        def->pinned ? def->base_url
                    : resolve(name, "base_url", computed_base ? computed_base : def->base_url);
    if (!base) {
        char *key = xasprintf("providers.%s.base_url", name);
        const struct config_setting *setting = config_setting_find(key);
        if (setting && setting->env_var)
            hax_err("provider '%s': no base_url (set %s, or %s in config.json)", name,
                    setting->env_var, key);
        else
            hax_err("provider '%s': no base_url (set %s in config.json)", name, key);
        free(key);
        free(computed_base);
        return NULL;
    }

    int model_discovered = 0;
    if (def->discover) {
        /* The hook talks to the server, so hand it the trimmed URL requests will use. */
        char *server_base = dup_trim_trailing_slash(base);
        int failed = def->discover(server_base, &model_discovered) != 0;
        free(server_base);
        if (failed) {
            free(computed_base);
            return NULL;
        }
    }

    /* Credentials must exist before anything else is built: a logged-out provider fails
     * construction with the hook's diagnostics, like a missing base_url. */
    struct http_auth_source auth = {0};
    if (def->auth_source && def->auth_source(def, &auth) != 0) {
        free(computed_base);
        return NULL;
    }

    char *default_model = NULL;
    char *default_effort = NULL;
    if (def->load_defaults)
        def->load_defaults(&default_model, &default_effort);

    /* Provider constructors copy any prefix-backed state before this buffer is freed. */
    char *cfg_prefix = xasprintf("providers.%s", name);
    /* Efforts are advisory offers narrowed by per-model metadata, so they default on; defs opt
     * out backends with no categorical effort. */
    int with_efforts = !def->no_efforts;
    json_t *def_extra_body = def->extra_body ? json_loads(def->extra_body, 0, NULL) : NULL;
    struct http_provider_preset preset = {
        .display_name = def->display_name ? def->display_name : name,
        .default_base_url = base,
        .api_key_env =
            def->pinned ? def->api_key_env : resolve(name, "api_key_env", def->api_key_env),
        .wire = wire,
        .pin_base_url = def->pinned,
        .cache = def->cache,
        .thinking_mode = def->thinking_mode,
        .strict_signatures = def->strict_signatures,
        .metadata_api = metadata_api_from_def(def),
        .send_cache_key_default = def->send_cache_key,
        .request_cost = def->request_cost,
        .reasoning_replay_field = def->reasoning_roundtrip,
        /* The def supplies only the default; the preset overlays <prefix>.reasoning_format
         * like every other quirk field. */
        .reasoning_format = chat_reasoning_format_parse(def->reasoning_format, CHAT_REASONING_FLAT),
        .efforts = with_efforts ? EFFORT_LADDER : NULL,
        .n_efforts = with_efforts ? EFFORT_LADDER_N : 0,
        .length_hint = def->length_hint,
        .config_prefix = cfg_prefix,
        .catalog_id = def->catalog_id,
        .parse_model = def->parse_model,
        .auth = auth,
        .default_model = default_model,
        .default_effort = default_effort,
        .extra_body = def_extra_body,
        /* model_apis or api "catalog" declares a mixed-protocol gateway, so catalog hints apply
         * there; a single-protocol provider keeps its explicit api regardless of catalog
         * metadata. */
        .catalog_wires = provider_routes_wires(name),
    };
    char **static_headers = def->static_headers ? def->static_headers() : NULL;
    preset.extra_headers = (const char *const *)static_headers;

    struct provider *p = http_provider_new_preset(&preset);
    string_array_free(static_headers);
    json_decref(def_extra_body);
    free(default_model);
    free(default_effort);
    free(cfg_prefix);
    free(computed_base);
    if (!p)
        return NULL;

    p->id = name;
    p->model_discovered = model_discovered;
    /* Like parse_model (which only the def's own listing consults), the probe and listing hooks
     * refine the def's metadata dialect: a configured metadata_api that moves the provider to
     * another dialect must keep that dialect's requests, not a mixed pairing. */
    if (http_provider_metadata_api(p) == def_metadata_api(def, wire)) {
        if (def->probe_model)
            p->probe_model = def->probe_model;
        if (def->list_models)
            p->list_models = def->list_models;
    }
    if (def->query_usage)
        p->query_usage = def->query_usage;
    if (def->model_label)
        p->model_label = def->model_label;

    /* Harmless without a probe hook; with one, it warms metadata for the active model. */
    const char *configured_model = config_str("model");
    model_meta_refresh(p,
                       configured_model && *configured_model ? configured_model : p->default_model);
    return p;
}

void provider_def_availability(const struct provider_def *def, struct provider_availability *out)
{
    const char *name = def->id;
    char *computed_base = def->pinned ? NULL : expand_base_url(def);
    const char *base =
        def->pinned ? def->base_url
                    : resolve(name, "base_url", computed_base ? computed_base : def->base_url);
    if (!base) {
        free(computed_base);
        out->available = 0;
        out->reason = xstrdup(def->unconfigured_reason ? def->unconfigured_reason : "no base_url");
        return;
    }

    const char *inline_key = cfg(name, "api_key");
    const char *key_env =
        def->pinned ? def->api_key_env : resolve(name, "api_key_env", def->api_key_env);
    if (inline_key || key_env) {
        free(computed_base);
        char *prefix = xasprintf("providers.%s", name);
        const char *api_key = provider_api_key(prefix, key_env);
        free(prefix);
        out->available = api_key != NULL;
        if (!out->available) {
            /* Name the exact variable or key to set. */
            out->reason = key_env ? xasprintf("%s not set", key_env)
                                  : xasprintf("providers.%s.api_key not set", name);
        }
        return;
    }

    /* Keyless without a probing def: configuration is the whole check, because a generic
     * endpoint may serve only its completion route and no /models. */
    if (!def->probe) {
        free(computed_base);
        out->available = 1;
        return;
    }

    /* Trim the trailing slash like the constructor: a base_url ending in "/" must probe
     * "<base>/models", not "//models", or a reachable server looks disabled. */
    char *probe_url = dup_trim_trailing_slash(base);
    char *prefix = xasprintf("providers.%s", name);
    char **extra_headers = provider_extra_headers(prefix);
    http_provider_prepare_base_url_availability(probe_url, NULL, extra_headers, out);
    string_array_free(extra_headers);
    free(prefix);
    free(probe_url);
    free(computed_base);
}
