/* SPDX-License-Identifier: MIT */
#include "providers/registry.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "diag.h"
#include "model_meta.h"
#include "provider.h"
#include "xalloc.h"
#include "providers/codex.h"
#include "providers/codex_auth.h"
#include "providers/codex_settings.h"
#include "providers/http_provider.h"
#include "providers/llamacpp.h"
#include "providers/mock.h"
#include "providers/opencode.h"
#include "providers/openrouter.h"

/* The shipped defs, user-facing ones first, in autoselect priority order. All are data plus
 * capability hooks built by the generic constructor; only the scripted mock, which speaks no
 * HTTP, constructs its own provider. */
// clang-format off
static const struct provider_def DEFS[] = {
    {
        .id = "codex",
        .api = "openai-responses",
        .base_url = "https://chatgpt.com/backend-api/codex",
        .pinned = 1,
        /* Subscription responses report no cost, so estimate against equivalent OpenAI API
         * rates; providers.codex.catalog_id renames or opts out of that identity. */
        .catalog_id = "openai",
        .send_cache_key = 1,
        /* The official client sends the catalog's per-model verbosity default, "low" for
         * effectively every served model; hax sends it flat rather than plumbing the flag. */
        .extra_body = "{\"text\": {\"verbosity\": \"low\"}}",
        .probe_model = codex_probe_model,
        .list_models = codex_list_models,
        .query_usage = codex_query_usage,
        .static_headers = codex_static_headers,
        .auth_source = codex_auth_source,
        .load_defaults = codex_load_settings,
        .prepare_availability = codex_prepare_availability,
    },
    {
        .id = "openai",
        .api = "openai-responses",
        .base_url = "https://api.openai.com/v1",
        .pinned = 1,
        .api_key_env = "OPENAI_API_KEY",
        .catalog_id = "openai",
        .send_cache_key = 1,
    },
    {
        .id = "anthropic",
        .api = "anthropic-messages",
        .base_url = "https://api.anthropic.com/v1",
        .pinned = 1,
        .api_key_env = "ANTHROPIC_API_KEY",
        .catalog_id = "anthropic",
        .cache = "on",
        .thinking_mode = "adaptive",
        .strict_signatures = 1,
    },
    {
        .id = "openrouter",
        .base_url = "https://openrouter.ai/api/v1",
        .pinned = 1,
        .api_key_env = "OPENROUTER_API_KEY",
        .send_cache_key = 1,
        /* OpenRouter requires explicit cache markers for routed Anthropic models. */
        .cache = "auto",
        .request_cost = 1,
        .reasoning_format = "nested",
        .parse_model = openrouter_parse_model,
        .probe_model = openrouter_probe_model,
        .query_usage = openrouter_query_usage,
        .static_headers = openrouter_static_headers,
    },
    /* One gateway family: the same key serves Zen (pay-as-you-go) and Go (subscription).
     * Models span all three wires; the catalog says which each one speaks, while the /models
     * side stays OpenAI-shaped regardless. */
    {
        .id = "opencode-zen",
        .api = "catalog",
        .base_url = "https://opencode.ai/zen/v1",
        .api_key_env = "OPENCODE_API_KEY",
        .catalog_id = "opencode",
        .metadata_api = "openai",
    },
    {
        .id = "opencode-go",
        .api = "catalog",
        .base_url = "https://opencode.ai/zen/go/v1",
        .api_key_env = "OPENCODE_API_KEY",
        .catalog_id = "opencode-go",
        .metadata_api = "openai",
        .query_usage = opencode_go_query_usage,
    },
    /* Local servers. */
    {
        /* Dot-free so the id names its providers.llamacpp config block ('.' is the config key
         * path separator); the banner and picker keep the upstream spelling. */
        .id = "llamacpp",
        .display_name = "llama.cpp",
        .base_url = "http://127.0.0.1:{port}/v1",
        /* Interleaved-thinking models can leak tool calls into reasoning unless prior
         * reasoning returns through llama-server's reasoning_content field. */
        .reasoning_roundtrip = "reasoning_content",
        /* Prompt-prefill progress for big local prompts; the parser always understands the
         * reply, so only the request member needs declaring. */
        .extra_body = "{\"return_progress\": true}",
        /* llama-server has no per-request context-size control. */
        .length_hint = "llama-server's context is full — restart it with a larger "
                       "-c / --ctx-size",
        /* Local reasoning is a per-model server toggle, not a categorical effort. */
        .no_efforts = 1,
        .parse_model = llamacpp_parse_model,
        .probe_model = llamacpp_probe_model,
        .model_label = llamacpp_model_label,
        .discover = llamacpp_discover,
        .prepare_availability = llamacpp_prepare_availability,
    },
    {
        .id = "ollama",
        .base_url = "http://127.0.0.1:{port}/v1",
        .port = 11434,
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
    /* The generic -compatible endpoints have no default base_url: unavailable until the user
     * supplies one, through the registered HAX_* env aliases or their providers.<name> block.
     * Last among the user-facing defs: a deliberately configured concrete provider should win
     * autoselect over a leftover generic base-URL variable. */
    {
        .id = "openai-compatible",
        .unconfigured_reason = "HAX_OPENAI_BASE_URL not set",
    },
    {
        .id = "anthropic-compatible",
        .api = "anthropic-messages",
        .unconfigured_reason = "HAX_ANTHROPIC_BASE_URL not set",
    },
    {
        .id = "mock",
        .internal = 1,
        .construct = mock_provider_new,
    },
};
// clang-format on
#define N_DEFS (sizeof(DEFS) / sizeof(DEFS[0]))

static const struct provider_def *shipped_find(const char *name)
{
    for (size_t i = 0; i < N_DEFS; i++)
        if (strcmp(name, DEFS[i].id) == 0)
            return &DEFS[i];
    return NULL;
}

/* Data-only defs for config.json providers.* names that match no shipped def (a matching block
 * overlays its shipped def at construction instead). Immutable after startup; built once. */
static const struct provider_def *const *config_defs(size_t *out_count)
{
    static struct provider_def **defs;
    static size_t count;
    static int built;
    if (!built) {
        char **names = NULL;
        size_t n_names = config_object_keys("providers", &names);
        defs = xcalloc(n_names ? n_names : 1, sizeof(*defs));
        for (size_t i = 0; i < n_names; i++) {
            /* '.' is the config key path separator, so a dotted name could never resolve its
             * providers.<name>.* fields; reject it rather than offer a provider that cannot
             * construct. */
            if (strchr(names[i], '.')) {
                hax_warn("ignoring custom provider '%s': name cannot contain '.'", names[i]);
            } else if (!shipped_find(names[i])) {
                struct provider_def *def = xcalloc(1, sizeof(*def));
                def->id = xstrdup(names[i]); /* process-lifetime; the registry never frees it */
                /* A shipped def curates its catalog identity or absence; a config-only def
                 * defaults to its own name. */
                def->catalog_id = def->id;
                defs[count++] = def;
            }
            free(names[i]);
        }
        free(names);
        built = 1;
    }
    *out_count = count;
    return (const struct provider_def *const *)defs;
}

const struct provider_def *provider_find(const char *name)
{
    if (!name)
        return NULL;
    name = provider_canonical_id(name);
    const struct provider_def *shipped = shipped_find(name);
    if (shipped)
        return shipped;
    size_t count;
    const struct provider_def *const *defs = config_defs(&count);
    for (size_t i = 0; i < count; i++)
        if (strcmp(name, defs[i]->id) == 0)
            return defs[i];
    return NULL;
}

const char *provider_display_name(const struct provider_def *def)
{
    char *key = xasprintf("providers.%s.display_name", def->id);
    const char *configured = config_str_nonempty(key);
    free(key);
    if (configured)
        return configured;
    return def->display_name ? def->display_name : def->id;
}

void provider_list_names(FILE *out)
{
    size_t count;
    const struct provider_def *const *defs = provider_all(&count);
    for (size_t i = 0; i < count; i++)
        fprintf(out, "%s%s", i ? " " : "", defs[i]->id);
}

const struct provider_def *const *provider_all(size_t *out_count)
{
    /* Config-defined defs are immutable after startup, so build the merged view once. */
    static const struct provider_def **defs;
    static size_t count;
    static int initialized;
    if (!initialized) {
        size_t config_count;
        const struct provider_def *const *config = config_defs(&config_count);
        defs = xcalloc(N_DEFS + config_count, sizeof(*defs));
        for (size_t i = 0; i < N_DEFS; i++)
            if (!DEFS[i].internal)
                defs[count++] = &DEFS[i];
        for (size_t i = 0; i < config_count; i++)
            defs[count++] = config[i];
        initialized = 1;
    }
    *out_count = count;
    return defs;
}

const struct provider_def *provider_default(void)
{
    size_t count;
    const struct provider_def *const *defs = provider_all(&count);
    return count ? defs[0] : NULL;
}

struct provider *provider_construct(const struct provider_def *def)
{
    struct provider *provider = def->construct ? def->construct(def) : http_provider_new(def);
    if (!provider)
        return NULL;
    /* Base config every provider honors, whichever constructor built it. */
    char *key = xasprintf("providers.%s.sort_models", def->id);
    provider->keep_model_order = !config_bool_or(key, 1);
    free(key);
    /* Warm metadata for the model the provider will serve first; harmless without a probe
     * hook. */
    const char *configured_model = config_str("model");
    model_meta_refresh(provider, configured_model && *configured_model ? configured_model
                                                                       : provider->default_model);
    return provider;
}

void provider_prepare_availability(const struct provider_def *def,
                                   struct provider_availability *out)
{
    memset(out, 0, sizeof(*out));
    if (def->prepare_availability) {
        def->prepare_availability(def, out);
        return;
    }
    /* A construct override without its own check leaves no data to check against. */
    if (def->construct) {
        out->available = 1;
        return;
    }
    http_provider_availability(def, out);
}
