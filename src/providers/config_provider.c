/* SPDX-License-Identifier: MIT */
#include "providers/config_provider.h"

#include <stdlib.h>
#include <string.h>

#include "anthropic.h"
#include "config.h"
#include "openai.h"
#include "util.h"

/* A built-in recipe: the default field values for a well-known provider,
 * overridable key-by-key by a matching providers.<name> config block. The
 * shipped set is deliberately small and slow-moving — only providers fully
 * described by static endpoint metadata (no auth/transport code), since hax
 * discovers models live via /v1/models rather than shipping a catalog.
 * Borrowed static strings throughout; the recipe table outlives every
 * provider built from it. */
struct provider_recipe {
    const char *name;             /* selectable id (HAX_PROVIDER value) */
    const char *display_name;     /* banner label; NULL → name */
    const char *api;              /* dialect: openai-completions | anthropic-messages */
    const char *base_url;         /* default endpoint */
    const char *api_key_env;      /* env var holding the key; NULL → local/no key */
    const char *reasoning_format; /* "flat"/"nested"; NULL → flat */
    int send_cache_key;           /* prompt_cache_key default (0/1) */
    const char *length_hint;      /* appended to a "length"-truncation error */
    int no_efforts;               /* suppress the OpenAI effort ladder for a
                                     local server whose thinking is a token
                                     budget, not a categorical level (so
                                     /effort skips it instead of persisting a
                                     value the backend can't honor) */
};

// clang-format off
static const struct provider_recipe RECIPES[] = {
    {
        .name = "ollama",
        .display_name = "ollama",
        .api = "openai-completions",
        /* Default endpoint; override with providers.ollama.base_url in
         * config.json (e.g. for a non-default port or a remote host). */
        .base_url = "http://127.0.0.1:11434/v1",
        /* ollama caps the runtime context at OLLAMA_CONTEXT_LENGTH (4096 by
         * default) and ignores a per-request num_ctx on its OpenAI endpoint,
         * so hax can't widen it — a prompt near that size truncates the reply
         * to "length". Point the user at the only real fix. */
        .length_hint = "ollama's context window may be too small for the prompt — "
                       "restart `ollama serve` with a larger OLLAMA_CONTEXT_LENGTH "
                       "(e.g. 16384), or raise num_ctx on the model",
        /* ollama has no categorical reasoning effort (thinking is a per-model
         * toggle/budget), so /effort should skip it rather than offer levels
         * it would send and the backend ignore. */
        .no_efforts = 1,
    },
};
// clang-format on
#define N_RECIPES (sizeof(RECIPES) / sizeof(RECIPES[0]))

static const struct provider_recipe *recipe_find(const char *name)
{
    for (size_t i = 0; i < N_RECIPES; i++)
        if (strcmp(RECIPES[i].name, name) == 0)
            return &RECIPES[i];
    return NULL;
}

/* Resolve providers.<name>.<leaf> from config (override → file/state; no env
 * tier for the named lane), empty counting as unset. Returns a borrowed
 * pointer into the config tier (process-lifetime) or NULL. */
static const char *cfg(const char *name, const char *leaf)
{
    char *k = xasprintf("providers.%s.%s", name, leaf);
    const char *v = config_str_nonempty(k);
    free(k);
    return v;
}

/* config value, else recipe field, else NULL. */
static const char *resolve(const char *name, const char *leaf, const char *recipe_val)
{
    const char *v = cfg(name, leaf);
    return v ? v : recipe_val;
}

/* Build the provider for config id `name` (the factory's own name). The
 * dialect (providers.<name>.api, else the recipe's, else openai-completions)
 * picks the construction path; every other field is resolved as config
 * overlaid on the recipe and threaded into the dialect's preset, which reads
 * its own subtree via config_prefix. */
static struct provider *config_provider_new(const char *name)
{
    const struct provider_recipe *r = recipe_find(name);

    const char *api = resolve(name, "api", r ? r->api : NULL);
    if (!api)
        api = "openai-completions";
    int is_openai = strcmp(api, "openai-completions") == 0;
    int is_anthropic = strcmp(api, "anthropic-messages") == 0;
    if (!is_openai && !is_anthropic) {
        hax_err("provider '%s': unsupported api '%s' "
                "(supported: openai-completions, anthropic-messages)",
                name, api);
        return NULL;
    }

    const char *base = resolve(name, "base_url", r ? r->base_url : NULL);
    if (!base || !*base) {
        hax_err("provider '%s': no base_url (set providers.%s.base_url in config.json)", name,
                name);
        return NULL;
    }

    const char *display = resolve(name, "display_name", r ? r->display_name : NULL);
    if (!display || !*display)
        display = name;

    /* The dialect's preset resolves the per-provider settings itself from this
     * prefix; the rest we pass as already-resolved fields. The prefix is read
     * synchronously during construction (the preset copies what it keeps), so
     * the stack-built string is safe to free on return. */
    char *cfg_prefix = xasprintf("providers.%s", name);

    if (is_anthropic) {
        /* Generic Anthropic Messages endpoint: budget-mode thinking, tolerant
         * of empty signatures, caching off — the compat-shim baseline, all
         * overridable per provider via providers.<name>.{thinking_mode,cache,…}. */
        struct anthropic_preset preset = {
            .display_name = display,
            .default_base_url = base,
            .api_key_env = resolve(name, "api_key_env", r ? r->api_key_env : NULL),
            .default_thinking_mode = ANTHROPIC_THINKING_BUDGET,
            .allow_empty_signature = 1,
            .send_cache_control_default = 0,
            .config_prefix = cfg_prefix,
        };
        struct provider *p = anthropic_provider_new_preset(&preset);
        free(cfg_prefix);
        return p;
    }

    const char *rf = resolve(name, "reasoning_format", r ? r->reasoning_format : NULL);

    /* The OpenAI effort ladder is advisory for a generic compat endpoint (the
     * user picked the URL), so it's the default — but a recipe can opt out for
     * a local server with no categorical effort (ollama). */
    int with_efforts = !(r && r->no_efforts);
    struct openai_preset preset = {
        .display_name = display,
        .default_base_url = base,
        .api_key_env = resolve(name, "api_key_env", r ? r->api_key_env : NULL),
        .send_cache_key_default = r ? r->send_cache_key : 0,
        .reasoning_format = reasoning_format_parse(rf, REASONING_FLAT),
        .efforts = with_efforts ? OPENAI_EFFORT_LADDER : NULL,
        .n_efforts = with_efforts ? OPENAI_EFFORT_LADDER_N : 0,
        .length_hint = r ? r->length_hint : NULL,
        .config_prefix = cfg_prefix,
    };
    struct provider *p = openai_provider_new_preset(&preset);
    free(cfg_prefix);
    return p;
}

/* Availability for the /provider picker. A keyed (cloud) provider — one with
 * a declared api_key_env or an inline api_key — is selectable iff that key
 * resolves, with no network probe (fast, and a 401 would be the only signal
 * anyway). A keyless one (a local server like ollama) is probed for
 * reachability, exactly as the constructor's endpoint would be reached.
 * Reason strings are static literals (this runs on a picker worker thread). */
static int config_provider_available(const char *name, const char **reason)
{
    const struct provider_recipe *r = recipe_find(name);

    const char *base = resolve(name, "base_url", r ? r->base_url : NULL);
    if (!base || !*base) {
        if (reason)
            *reason = "no base_url";
        return 0;
    }

    const char *key_env = resolve(name, "api_key_env", r ? r->api_key_env : NULL);
    const char *inline_key = cfg(name, "api_key");
    const char *key = inline_key;
    if ((!key || !*key) && key_env && *key_env)
        key = getenv(key_env);

    int keyed = (key_env && *key_env) || (inline_key && *inline_key);
    if (keyed) {
        if (key && *key)
            return 1;
        if (reason)
            *reason = "API key not set";
        return 0;
    }
    /* Trim the trailing slash the constructor also trims, so the probe targets
     * the same "<base>/models" the running provider would — otherwise a
     * base_url ending in "/" probes "//models" and a reachable server looks
     * disabled. */
    char *probe = dup_trim_trailing_slash(base);
    int ok = openai_base_url_reachable(probe, (key && *key) ? key : NULL, reason);
    free(probe);
    return ok;
}

static struct provider_factory *make_factory(const char *name)
{
    struct provider_factory *f = xcalloc(1, sizeof(*f));
    f->name = xstrdup(name); /* process-lifetime; the registry never frees these */
    f->new = config_provider_new;
    f->available = config_provider_available;
    return f;
}

const struct provider_factory *const *config_providers(size_t *n)
{
    static const struct provider_factory **view;
    static size_t count;
    static int built;
    if (!built) {
        char **names = NULL;
        size_t n_cfg = config_object_keys("providers", &names);
        view = xcalloc(N_RECIPES + n_cfg, sizeof(*view));
        /* Recipes first (stable, shipped order), then config-only names. A
         * config block whose name matches a recipe is an overlay, resolved at
         * construction — not a second factory — so skip it here. */
        for (size_t i = 0; i < N_RECIPES; i++)
            view[count++] = make_factory(RECIPES[i].name);
        for (size_t i = 0; i < n_cfg; i++) {
            if (recipe_find(names[i])) {
                free(names[i]); /* overlay of a recipe, already added */
                continue;
            }
            /* The name doubles as the providers.<name>.* config-key segment,
             * and '.' is the key path separator — so a dotted name (e.g.
             * "my.llm") would resolve its fields as providers→my→llm→… and
             * never find them. Reject it loudly rather than offer a provider
             * that can't construct; the user picks a dot-free name (use '-'). */
            if (strchr(names[i], '.'))
                hax_warn("ignoring custom provider '%s': name cannot contain '.'", names[i]);
            else
                view[count++] = make_factory(names[i]);
            free(names[i]);
        }
        free(names);
        built = 1;
    }
    *n = count;
    return view;
}
