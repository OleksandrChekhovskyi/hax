/* SPDX-License-Identifier: MIT */
#include "providers/config_provider.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "provider.h"
#include "util.h"
#include "providers/anthropic.h"
#include "providers/openai.h"
#include "providers/openai_messages.h"

/* A built-in recipe: the default field values for a well-known provider, overridable
 * key-by-key by a matching providers.<name> config block. The shipped set is deliberately
 * small and slow-moving — only providers fully described by static endpoint metadata (no
 * auth/transport code), since hax discovers models live via /v1/models rather than shipping
 * a catalog. Borrowed static strings throughout; the recipe table outlives every provider
 * built from it. */
struct provider_recipe {
    const char *id;               /* selectable HAX_PROVIDER value */
    const char *display_name;     /* banner label; NULL → name */
    const char *api;              /* dialect: openai-completions | openai-responses |
                                     anthropic-messages */
    const char *base_url;         /* default endpoint; NULL → the user must configure one */
    const char *api_key_env;      /* env var holding the key; NULL → local/no key */
    const char *reasoning_format; /* "flat"/"nested"; NULL → flat */
    const char *catalog_id;       /* models.dev key (catalog.h); in a recipe, NULL is a
                                     curated absence — no fallback to `name` */
    int send_cache_key;           /* prompt_cache_key default (0/1) */
    const char *length_hint;      /* appended to a "length"-truncation error */
    int no_efforts;               /* offer no effort levels, so /effort skips the provider */
    /* Probe <base_url>/models reachability when keyless. Only for curated local recipes where
     * "not running" is the common failure and /models is known to exist; a generic endpoint may
     * not serve /models at all, so configuration is the default availability check. */
    int probe;
    const char *unconfigured_reason; /* availability reason without a base_url; NULL →
                                        "no base_url" */
};

// clang-format off
static const struct provider_recipe RECIPES[] = {
    /* The generic -compatible endpoints are recipes with no default base_url: unavailable
     * until the user supplies one, through the registered HAX_* env aliases or their
     * providers.<name> block. */
    {
        .id = "openai-compatible",
        .api = "openai-completions",
        .unconfigured_reason = "HAX_OPENAI_BASE_URL not set",
    },
    {
        .id = "anthropic-compatible",
        .api = "anthropic-messages",
        .unconfigured_reason = "HAX_ANTHROPIC_BASE_URL not set",
    },
    {
        .id = "ollama",
        .api = "openai-completions",
        .base_url = "http://127.0.0.1:11434/v1",
        /* ollama caps the runtime context at OLLAMA_CONTEXT_LENGTH (4096 by default) and
         * ignores a per-request num_ctx on its OpenAI endpoint, so hax can't widen it — a
         * prompt near that size truncates the reply to "length". Point the user at the only
         * real fix. */
        .length_hint = "ollama's context window may be too small for the prompt — "
                       "restart `ollama serve` with a larger OLLAMA_CONTEXT_LENGTH "
                       "(e.g. 16384), or raise num_ctx on the model",
        /* ollama's thinking is a per-model toggle/budget, not a categorical effort, and its
         * local models aren't the hosted ones the catalog describes: no effort ladder, no
         * catalog_id. */
        .no_efforts = 1,
        /* A local daemon that reliably serves /models: worth dimming in /provider when down. */
        .probe = 1,
    },
};
// clang-format on
#define N_RECIPES (sizeof(RECIPES) / sizeof(RECIPES[0]))

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

const char *provider_api_key(const char *config_prefix, const char *api_key_env)
{
    const char *key = config_scoped_str(config_prefix, "api_key");
    if (key && *key)
        return key;
    key = api_key_env ? getenv(api_key_env) : NULL;
    return key && *key ? key : NULL;
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
void provider_warn_unused_fields(const char *name, const char *api_label, unsigned dialects,
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

void provider_warn_unused_openai_fields(const char *name, enum openai_wire default_wire,
                                        const char *const *extra)
{
    char *key = xasprintf("providers.%s.api", name);
    enum openai_wire wire = openai_wire_parse(config_str(key), default_wire);
    free(key);
    if (wire == OPENAI_WIRE_RESPONSES)
        provider_warn_unused_fields(name, "openai-responses", PROVIDER_FIELD_OPENAI_RESPONSES,
                                    extra);
    else
        provider_warn_unused_fields(name, "openai-completions", PROVIDER_FIELD_OPENAI_CHAT, extra);
}

/* Stands in for a missing recipe so field lookups need no NULL checks; the NULL `id`
 * still marks the recipe's absence where it matters (resolve_catalog_id). */
static const struct provider_recipe NO_RECIPE = {0};

static const struct provider_recipe *recipe_find(const char *name)
{
    for (size_t i = 0; i < N_RECIPES; i++)
        if (strcmp(RECIPES[i].id, name) == 0)
            return &RECIPES[i];
    return &NO_RECIPE;
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
    p->sort_models = config_bool_or(key, 0);
    free(key);
    return p;
}

/* Build the provider for config id `name` (the factory's own name). The dialect
 * (providers.<name>.api, else the recipe's, else openai-completions) picks the preset
 * constructor; every other field resolves as config overlaid on the recipe, and the preset
 * reads its dialect-specific keys from the same subtree via config_prefix. */
static struct provider *config_provider_new(const char *name)
{
    const struct provider_recipe *r = recipe_find(name);

    const char *api = resolve(name, "api", r->api);
    if (!api)
        api = "openai-completions";
    /* Case-insensitive like openai_wire_parse and the registry choices it feeds. "chat" and
     * "responses" are the short spellings HAX_OPENAI_API documents. */
    int is_anthropic = strcasecmp(api, "anthropic-messages") == 0;
    if (!is_anthropic && strcasecmp(api, "openai-completions") != 0 &&
        strcasecmp(api, "chat") != 0 && strcasecmp(api, "openai-responses") != 0 &&
        strcasecmp(api, "responses") != 0) {
        hax_err("provider '%s': unsupported api '%s' "
                "(supported: openai-completions, openai-responses, anthropic-messages)",
                name, api);
        return NULL;
    }
    unsigned dialect = PROVIDER_FIELD_ANTHROPIC;
    if (!is_anthropic) {
        dialect = openai_wire_parse(api, OPENAI_WIRE_CHAT) == OPENAI_WIRE_RESPONSES
                      ? PROVIDER_FIELD_OPENAI_RESPONSES
                      : PROVIDER_FIELD_OPENAI_CHAT;
    }
    provider_warn_unused_fields(name, api, dialect | PROVIDER_FIELD_CONFIG_DEFINED, NULL);

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
    struct provider *p;
    if (is_anthropic) {
        /* Generic endpoints default to compat-safe thinking and caching behavior. */
        struct anthropic_preset preset = {
            .display_name = display,
            .default_base_url = base,
            .api_key_env = api_key_env,
            .default_thinking_mode = ANTHROPIC_THINKING_BUDGET,
            .allow_empty_signature = 1,
            .send_cache_control_default = 0,
            .config_prefix = cfg_prefix,
            .catalog_id = resolve_catalog_id(name, r),
        };
        p = anthropic_provider_new_preset(&preset);
    } else {
        /* The OpenAI effort ladder is advisory for a generic compat endpoint (the user
         * picked the URL), so it defaults on; a recipe opts out for a backend with no
         * categorical effort. */
        int with_efforts = !r->no_efforts;
        struct openai_preset preset = {
            .display_name = display,
            .default_base_url = base,
            .api_key_env = api_key_env,
            .wire = openai_wire_parse(api, OPENAI_WIRE_CHAT),
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
        };
        p = openai_provider_new_preset(&preset);
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
    const struct provider_recipe *r = recipe_find(name);

    const char *base = resolve(name, "base_url", r->base_url);
    if (!base) {
        out->available = 0;
        out->reason = r->unconfigured_reason ? r->unconfigured_reason : "no base_url";
        return;
    }

    const char *inline_key = cfg(name, "api_key");
    const char *key_env = resolve(name, "api_key_env", r->api_key_env);
    if (inline_key || key_env) {
        const char *key = inline_key ? inline_key : getenv(key_env);
        out->available = key && *key;
        out->reason = out->available ? NULL : "API key not set";
        return;
    }

    /* Keyless without a probing recipe: configuration is the whole check, because a generic
     * endpoint may serve only its completion route and no /models. */
    if (!r->probe) {
        out->available = 1;
        return;
    }

    /* Trim the trailing slash the constructor also trims, so the probe targets the same
     * "<base>/models" the running provider would — a base_url ending in "/" must not probe
     * "//models" and make a reachable server look disabled. */
    char *probe_url = dup_trim_trailing_slash(base);
    openai_prepare_base_url_availability(probe_url, NULL, out);
    free(probe_url);
}

static struct provider_factory *make_factory(const char *name)
{
    struct provider_factory *f = xcalloc(1, sizeof(*f));
    f->id = xstrdup(name); /* process-lifetime; the registry never frees these */
    f->display_name = recipe_find(name)->display_name;
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
        factories = xcalloc(N_RECIPES + n_cfg, sizeof(*factories));
        /* Recipes first, in shipped order; then config-only names. A config block matching a
         * recipe name overlays that recipe at construction — it is not a second factory. */
        for (size_t i = 0; i < N_RECIPES; i++)
            factories[count++] = make_factory(RECIPES[i].id);
        for (size_t i = 0; i < n_cfg; i++) {
            /* '.' is the config key path separator, so a dotted name could never resolve
             * its providers.<name>.* fields; reject it rather than offer a provider that
             * cannot construct. */
            if (strchr(names[i], '.'))
                hax_warn("ignoring custom provider '%s': name cannot contain '.'", names[i]);
            else if (!recipe_find(names[i])->id)
                factories[count++] = make_factory(names[i]);
            free(names[i]);
        }
        free(names);
        built = 1;
    }
    *n = count;
    return factories;
}
