/* SPDX-License-Identifier: MIT */
#include "providers/config_provider.h"

#include <stdlib.h>
#include <string.h>

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
    const char *name;             /* selectable id (HAX_PROVIDER value) */
    const char *display_name;     /* banner label; NULL → name */
    const char *api;              /* dialect: openai-completions | anthropic-messages */
    const char *base_url;         /* default endpoint */
    const char *api_key_env;      /* env var holding the key; NULL → local/no key */
    const char *reasoning_format; /* "flat"/"nested"; NULL → flat */
    const char *catalog_id;       /* models.dev key (catalog.h); in a recipe, NULL is a
                                     curated absence — no fallback to `name` */
    int send_cache_key;           /* prompt_cache_key default (0/1) */
    const char *length_hint;      /* appended to a "length"-truncation error */
    int no_efforts;               /* offer no effort levels, so /effort skips the provider */
};

// clang-format off
static const struct provider_recipe RECIPES[] = {
    {
        .name = "ollama",
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
    },
};
// clang-format on
#define N_RECIPES (sizeof(RECIPES) / sizeof(RECIPES[0]))

/* Stands in for a missing recipe so field lookups need no NULL checks; the NULL `name`
 * still marks the recipe's absence where it matters (resolve_catalog_id). */
static const struct provider_recipe NO_RECIPE = {0};

static const struct provider_recipe *recipe_find(const char *name)
{
    for (size_t i = 0; i < N_RECIPES; i++)
        if (strcmp(RECIPES[i].name, name) == 0)
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
    return r->name ? r->catalog_id : name;
}

/* Dialect-agnostic base-provider fields are resolved here, once, from the providers.<name>
 * subtree; dialect constructors resolve only keys specific to their wire format. */
static struct provider *apply_base_config(const char *name, struct provider *p)
{
    if (!p)
        return NULL;
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
    int is_anthropic = strcmp(api, "anthropic-messages") == 0;
    if (!is_anthropic && strcmp(api, "openai-completions") != 0) {
        hax_err("provider '%s': unsupported api '%s' "
                "(supported: openai-completions, anthropic-messages)",
                name, api);
        return NULL;
    }

    const char *base = resolve(name, "base_url", r->base_url);
    if (!base) {
        hax_err("provider '%s': no base_url (set providers.%s.base_url in config.json)", name,
                name);
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
        const char *rf = resolve(name, "reasoning_format", r->reasoning_format);
        struct openai_preset preset = {
            .display_name = display,
            .default_base_url = base,
            .api_key_env = api_key_env,
            .send_cache_key_default = r->send_cache_key,
            .reasoning_format = openai_reasoning_format_parse(rf, OPENAI_REASONING_FLAT),
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
 * probe (fast, and a 401 would be the only extra signal). A keyless one (a local server
 * like ollama) is probed for reachability. */
static void config_provider_prepare_availability(const char *name,
                                                 struct provider_availability *out)
{
    const struct provider_recipe *r = recipe_find(name);

    const char *base = resolve(name, "base_url", r->base_url);
    if (!base) {
        out->available = 0;
        out->reason = "no base_url";
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
    f->name = xstrdup(name); /* process-lifetime; the registry never frees these */
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
            factories[count++] = make_factory(RECIPES[i].name);
        for (size_t i = 0; i < n_cfg; i++) {
            /* '.' is the config key path separator, so a dotted name could never resolve
             * its providers.<name>.* fields; reject it rather than offer a provider that
             * cannot construct. */
            if (strchr(names[i], '.'))
                hax_warn("ignoring custom provider '%s': name cannot contain '.'", names[i]);
            else if (!recipe_find(names[i])->name)
                factories[count++] = make_factory(names[i]);
            free(names[i]);
        }
        free(names);
        built = 1;
    }
    *n = count;
    return factories;
}
