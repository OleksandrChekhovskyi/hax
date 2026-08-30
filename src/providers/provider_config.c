/* SPDX-License-Identifier: MIT */
#include "providers/provider_config.h"

#include <ctype.h>
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "diag.h"
#include "trace.h"
#include "util.h"

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

/* A silently ignored member hides a typo or a dialect mix-up. */
void provider_warn_unused_fields(const char *name, const char *api_label, unsigned classes,
                                 const char *const *extra)
{
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
