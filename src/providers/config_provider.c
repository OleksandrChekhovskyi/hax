/* SPDX-License-Identifier: MIT */
#include "providers/config_provider.h"

#include <ctype.h>
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "provider.h"
#include "trace.h"
#include "util.h"
#include "providers/anthropic.h"
#include "providers/anthropic_messages.h"
#include "providers/http_provider.h"
#include "providers/openai_messages.h"
#include "providers/recipes.h"
#include "providers/wire.h"

#define FIELD_ANY (PROVIDER_FIELD_OPENAI | PROVIDER_FIELD_ANTHROPIC)
/* Responses fixes its reasoning shape and round-trip on the wire and never sends explicit cache
 * markers, so those knobs apply to Chat Completions only. */
#define FIELD_CACHE_MARKERS (PROVIDER_FIELD_OPENAI_CHAT | PROVIDER_FIELD_ANTHROPIC)

// clang-format off
static const struct provider_field PROVIDER_FIELDS[] = {
    /* `api` picks a config-defined provider's dialect and an OpenAI-family provider's wire; the
     * compiled-in anthropic provider has no wire choice, so it must warn there. */
    {.leaf = "api",                 .dialects = PROVIDER_FIELD_OPENAI | PROVIDER_FIELD_CONFIG_DEFINED},
    {.leaf = "base_url",            .dialects = FIELD_ANY},
    {.leaf = "api_key",             .dialects = FIELD_ANY, .secret = 1},
    {.leaf = "api_key_env",         .dialects = PROVIDER_FIELD_CONFIG_DEFINED},
    {.leaf = "display_name",        .dialects = FIELD_ANY},
    {.leaf = "catalog_id",          .dialects = PROVIDER_FIELD_CONFIG_DEFINED},
    {.leaf = "sort_models",         .dialects = PROVIDER_FIELD_CONFIG_DEFINED},
    {.leaf = "model_apis",          .dialects = FIELD_ANY},
    {.leaf = "cache",               .dialects = FIELD_CACHE_MARKERS},
    {.leaf = "cache_ttl",           .dialects = FIELD_CACHE_MARKERS},
    {.leaf = "send_cache_key",      .dialects = PROVIDER_FIELD_OPENAI},
    {.leaf = "request_cost",        .dialects = PROVIDER_FIELD_OPENAI_CHAT},
    {.leaf = "reasoning_format",    .dialects = PROVIDER_FIELD_OPENAI_CHAT},
    {.leaf = "reasoning_roundtrip", .dialects = PROVIDER_FIELD_OPENAI_CHAT},
    {.leaf = "max_tokens",          .dialects = PROVIDER_FIELD_ANTHROPIC},
    {.leaf = "thinking_mode",       .dialects = PROVIDER_FIELD_ANTHROPIC},
    {.leaf = "thinking_budget",     .dialects = PROVIDER_FIELD_ANTHROPIC},
    {.leaf = "version",             .dialects = PROVIDER_FIELD_ANTHROPIC},
    {.leaf = "extra_body",          .dialects = FIELD_ANY},
    {.leaf = "extra_headers",       .dialects = FIELD_ANY, .secret = 1},
};
// clang-format on
#define N_PROVIDER_FIELDS (sizeof(PROVIDER_FIELDS) / sizeof(PROVIDER_FIELDS[0]))

const struct provider_field *provider_fields(size_t *n)
{
    *n = N_PROVIDER_FIELDS;
    return PROVIDER_FIELDS;
}

static unsigned provider_wire_dialects(const struct wire *wire)
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

/* A model_apis block or api "catalog" — configured or from the recipe — makes the provider a
 * mixed-protocol gateway: models may land on any wire, so every dialect's fields are live. */
static int provider_routes_wires(const char *name)
{
    char *key = xasprintf("providers.%s.model_apis", name);
    const json_t *rules = config_json_node(key);
    free(key);
    if (json_is_object(rules) && json_object_size((json_t *)rules) > 0)
        return 1;
    const char *api = cfg(name, "api");
    if (!api)
        api = provider_recipe_find(name)->api;
    return api && strcasecmp(api, "catalog") == 0;
}

/* A silently ignored member hides a typo or a dialect mix-up. */
void provider_warn_unused_fields(const char *name, const struct wire *wire, unsigned extra_dialects,
                                 const char *const *extra)
{
    const char *api_label = wire ? wire->id : name;
    unsigned dialects = (wire ? provider_wire_dialects(wire) : 0) | extra_dialects;
    if (provider_routes_wires(name))
        dialects |= FIELD_ANY;
    char *key = xasprintf("providers.%s", name);
    char **members = NULL;
    size_t n_members = config_object_keys(key, &members);
    free(key);
    for (size_t i = 0; i < n_members; i++) {
        const struct provider_field *field = field_find(members[i]);
        if (string_list_has(extra, members[i])) {
            ; /* consumed by this provider */
        } else if (field) {
            if (field->dialects & dialects) {
                ; /* consumed by this provider */
            } else if (field->dialects & ~PROVIDER_FIELD_CONFIG_DEFINED) {
                /* The dialect wording wins whenever some wire does consume the field. */
                hax_warn("provider '%s': field '%s' is not used by %s providers", name, members[i],
                         api_label);
            } else {
                hax_warn("provider '%s': field '%s' applies only to config-defined providers", name,
                         members[i]);
            }
        } else if (!module_key_registered(name, members[i])) {
            hax_warn("provider '%s': unknown field '%s' (see docs/providers.md)", name, members[i]);
        }
        free(members[i]);
    }
    free(members);
}

void provider_warn_unused_wire_fields(const char *name, const struct wire *default_wire,
                                      const char *const *extra)
{
    const struct wire *wire = default_wire;
    if (default_wire != &WIRE_ANTHROPIC_MESSAGES) {
        char *key = xasprintf("providers.%s.api", name);
        wire = wire_find(config_str(key));
        free(key);
        if (!wire || wire == &WIRE_ANTHROPIC_MESSAGES)
            wire = default_wire;
    }
    provider_warn_unused_fields(name, wire, 0, extra);
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

/* Config value, else recipe field, else NULL; never empty. */
static const char *resolve(const char *name, const char *leaf, const char *recipe_val)
{
    const char *v = cfg(name, leaf);
    return v ? v : recipe_val;
}

/* An explicit catalog_id wins, including an empty opt-out. A recipe keeps its curated
 * identity or absence; a recipe-less provider defaults to its own name. The result is
 * borrowed only until construction returns because a config write may invalidate it. */
static const char *resolve_catalog_id(const char *name, const struct provider_recipe *r)
{
    char *key = xasprintf("providers.%s.catalog_id", name);
    const char *v = config_str(key);
    free(key);
    if (v)
        return *v ? v : NULL;
    return r->id ? r->catalog_id : name;
}

/* Dialect-agnostic base-provider fields are resolved here, once, from the providers.<name>
 * subtree; dialect constructors resolve only keys specific to their wire format. */
static struct provider *apply_base_config(const char *name, struct provider *p)
{
    if (!p)
        return NULL;
    /* `name` is the factory's id, which the registry keeps alive for the process. */
    p->id = name;
    char *key = xasprintf("providers.%s.sort_models", name);
    p->keep_model_order = !config_bool_or(key, 1);
    free(key);
    return p;
}

/* Build the provider for config id `name` (the factory's own name). The api field —
 * config, else recipe, else openai-completions — picks the wire; every other field resolves
 * as config overlaid on the recipe, with dialect-specific keys read from the same subtree
 * via config_prefix. */
static struct provider *config_provider_new(const char *name)
{
    const struct provider_recipe *r = provider_recipe_find(name);

    const char *api = resolve(name, "api", r->api);
    if (!api)
        api = "openai-completions";
    const struct wire *wire;
    if (strcasecmp(api, "catalog") == 0) {
        /* Per-model routing from catalog hints; models the catalog leaves unmapped use Chat
         * Completions, like the shipped gateway recipes. */
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
    provider_warn_unused_fields(name, wire, PROVIDER_FIELD_CONFIG_DEFINED, NULL);

    const char *base = resolve(name, "base_url", r->base_url);
    if (!base) {
        char *key = xasprintf("providers.%s.base_url", name);
        const struct config_setting *setting = config_setting_find(key);
        if (setting && setting->env_var)
            hax_err("provider '%s': no base_url (set %s, or %s in config.json)", name,
                    setting->env_var, key);
        else
            hax_err("provider '%s': no base_url (set %s in config.json)", name, key);
        free(key);
        return NULL;
    }

    const char *display = resolve(name, "display_name", r->display_name);
    if (!display)
        display = name;
    const char *api_key_env = resolve(name, "api_key_env", r->api_key_env);

    /* Provider constructors copy any prefix-backed state before this buffer is freed. */
    char *cfg_prefix = xasprintf("providers.%s", name);
    /* Efforts are advisory for a generic endpoint, so they default on; recipes opt out
     * backends with no categorical effort, and the Messages constructor swaps in its ladder. */
    int with_efforts = !r->no_efforts;
    struct http_provider_preset preset = {
        .display_name = display,
        .default_base_url = base,
        .api_key_env = api_key_env,
        .wire = wire,
        .send_cache_key_default = r->send_cache_key,
        /* The recipe supplies only the default; the preset overlays <prefix>.reasoning_format
         * like every other quirk field. */
        .reasoning_format =
            openai_reasoning_format_parse(r->reasoning_format, OPENAI_REASONING_FLAT),
        .efforts = with_efforts ? OPENAI_EFFORT_LADDER : NULL,
        .n_efforts = with_efforts ? OPENAI_EFFORT_LADDER_N : 0,
        .length_hint = r->length_hint,
        .config_prefix = cfg_prefix,
        .catalog_id = resolve_catalog_id(name, r),
        /* model_apis or api "catalog" declares a mixed-protocol gateway, so catalog hints apply
         * there; a single-protocol provider keeps its explicit api regardless of catalog
         * metadata. */
        .catalog_wires = provider_routes_wires(name),
    };
    if (strcasecmp(api, "catalog") == 0 && !preset.catalog_id)
        hax_warn("provider '%s': api \"catalog\" routes by catalog metadata, but catalog_id "
                 "is empty",
                 name);

    struct provider *p;
    if (wire == &WIRE_ANTHROPIC_MESSAGES) {
        /* Generic endpoints default to compat-safe thinking and caching behavior. */
        preset.default_thinking_mode = ANTHROPIC_THINKING_BUDGET;
        preset.allow_empty_signature = 1;
        p = anthropic_provider_new_preset(&preset);
    } else {
        /* A gateway's Messages-wire models get the same compat-safe defaults. */
        if (provider_routes_wires(name)) {
            preset.default_thinking_mode = ANTHROPIC_THINKING_BUDGET;
            preset.allow_empty_signature = 1;
        }
        p = http_provider_new_preset(&preset);
    }
    free(cfg_prefix);
    return apply_base_config(name, p);
}

/* Availability for the /provider picker. A keyed (cloud) provider — one with a declared
 * api_key_env or an inline api_key — is selectable iff that key resolves, with no network
 * probe (fast, and a 401 would be the only extra signal). A keyless one counts its configured
 * base_url as availability, except for recipes that opt into a reachability probe. */
static void config_provider_prepare_availability(const char *name,
                                                 struct provider_availability *out)
{
    const struct provider_recipe *r = provider_recipe_find(name);

    const char *base = resolve(name, "base_url", r->base_url);
    if (!base) {
        out->available = 0;
        out->reason = xstrdup(r->unconfigured_reason ? r->unconfigured_reason : "no base_url");
        return;
    }

    const char *inline_key = cfg(name, "api_key");
    const char *key_env = resolve(name, "api_key_env", r->api_key_env);
    if (inline_key || key_env) {
        char *prefix = xasprintf("providers.%s", name);
        const char *api_key = provider_api_key(prefix, key_env);
        free(prefix);
        out->available = api_key != NULL;
        if (!out->available) {
            /* Name the exact variable or key to set, like the compiled-in providers do. */
            out->reason = key_env ? xasprintf("%s not set", key_env)
                                  : xasprintf("providers.%s.api_key not set", name);
        }
        return;
    }

    /* Keyless without a probing recipe: configuration is the whole check, because a generic
     * endpoint may serve only its completion route and no /models. */
    if (!r->probe) {
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
}

static struct provider_factory *make_factory(const char *name)
{
    struct provider_factory *f = xcalloc(1, sizeof(*f));
    f->id = xstrdup(name); /* process-lifetime; the registry never frees these */
    f->display_name = provider_recipe_find(name)->display_name;
    f->new = config_provider_new;
    f->prepare_availability = config_provider_prepare_availability;
    return f;
}

const struct provider_factory *const *config_providers(size_t *n)
{
    static const struct provider_factory **factories;
    static size_t count;
    static int built;
    if (!built) {
        char **names = NULL;
        size_t n_cfg = config_object_keys("providers", &names);
        size_t n_recipes;
        const struct provider_recipe *recipes = provider_recipes(&n_recipes);
        factories = xcalloc(n_recipes + n_cfg, sizeof(*factories));
        /* Recipes first, in shipped order; then config-only names. A config block matching a
         * recipe name overlays that recipe at construction — it is not a second factory. */
        for (size_t i = 0; i < n_recipes; i++)
            factories[count++] = make_factory(recipes[i].id);
        for (size_t i = 0; i < n_cfg; i++) {
            /* '.' is the config key path separator, so a dotted name could never resolve
             * its providers.<name>.* fields; reject it rather than offer a provider that
             * cannot construct. */
            if (strchr(names[i], '.'))
                hax_warn("ignoring custom provider '%s': name cannot contain '.'", names[i]);
            else if (!provider_recipe_find(names[i])->id)
                factories[count++] = make_factory(names[i]);
            free(names[i]);
        }
        free(names);
        built = 1;
    }
    *n = count;
    return factories;
}
