/* SPDX-License-Identifier: MIT */
#include "providers/recipes.h"

#include <stddef.h>
#include <string.h>

#include "providers/opencode.h"

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
    /* One gateway family: the same key serves Zen (pay-as-you-go) and Go (subscription).
     * Models span all three wires; the catalog says which each one speaks. */
    {
        .id = "opencode-zen",
        .api = "catalog",
        .base_url = "https://opencode.ai/zen/v1",
        .api_key_env = "OPENCODE_API_KEY",
        .catalog_id = "opencode",
    },
    {
        .id = "opencode-go",
        .api = "catalog",
        .base_url = "https://opencode.ai/zen/go/v1",
        .api_key_env = "OPENCODE_API_KEY",
        .catalog_id = "opencode-go",
        .query_usage = opencode_go_query_usage,
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

static const struct provider_recipe NO_RECIPE = {0};

const struct provider_recipe *provider_recipes(size_t *n)
{
    *n = N_RECIPES;
    return RECIPES;
}

const struct provider_recipe *provider_recipe_find(const char *id)
{
    for (size_t i = 0; i < N_RECIPES; i++)
        if (strcmp(RECIPES[i].id, id) == 0)
            return &RECIPES[i];
    return &NO_RECIPE;
}
