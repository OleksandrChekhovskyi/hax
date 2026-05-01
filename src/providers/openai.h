/* SPDX-License-Identifier: MIT */
#ifndef HAX_OPENAI_H
#define HAX_OPENAI_H

#include "provider.h"

/* Real OpenAI Chat Completions: locked to https://api.openai.com/v1. Reads:
 *   HAX_OPENAI_API_KEY   — preferred; falls back to OPENAI_API_KEY
 *   HAX_PROVIDER_NAME    — optional display name (defaults to "openai")
 *
 * Rejects HAX_OPENAI_BASE_URL — for custom OpenAI-compatible endpoints, use
 * the openai-compatible preset (openai_compat.h) instead, which keeps
 * OPENAI_API_KEY and prompt_cache_key scoped to the real-OpenAI host.
 *
 * Returns NULL on failure (prints cause to stderr). */
struct provider *openai_provider_new(void);

/* Preset configuration consumed by openai_provider_new_preset(). All fields
 * are optional except `default_base_url` (or HAX_OPENAI_BASE_URL must be
 * set). Used by the thin shim providers (openai-compatible, llama.cpp,
 * openrouter, …) that all speak the same Chat Completions translation but
 * want different defaults, auth fallbacks, and request headers. */
struct openai_preset {
    /* Display name when HAX_PROVIDER_NAME is unset. NULL → "openai". */
    const char *display_name;
    /* Default base URL when HAX_OPENAI_BASE_URL is unset. NULL is allowed
     * only when the preset requires HAX_OPENAI_BASE_URL (the constructor
     * fails if neither resolves to a non-empty URL). */
    const char *default_base_url;
    /* Optional second env var consulted for the API key, after
     * HAX_OPENAI_API_KEY. Each preset declares which global it picks up
     * (OPENAI_API_KEY for openai, OPENROUTER_API_KEY for openrouter, …);
     * unset for openai-compatible so a global OPENAI_API_KEY isn't
     * forwarded to an unrelated third-party endpoint. NULL → none. */
    const char *api_key_env;
    /* Whether to send prompt_cache_key by default. 0 = off (local servers,
     * generic compat backends), 1 = on (real OpenAI, OpenRouter, other
     * hosted compat backends with prefix caching).
     * HAX_OPENAI_SEND_CACHE_KEY=<anything> forces on. */
    int send_cache_key_default;
    /* Optional extra request headers, NULL-terminated array of
     * "Name: value" strings. Copied at construction; the preset does not
     * need to keep them alive afterwards. NULL → none. */
    const char *const *extra_headers;
};

/* Build an OpenAI-compatible provider configured by `preset`. NULL preset
 * is equivalent to a zero-initialized one (which will fail unless
 * HAX_OPENAI_BASE_URL is set). Returns NULL on failure. */
struct provider *openai_provider_new_preset(const struct openai_preset *preset);

extern const struct provider_factory PROVIDER_OPENAI;

#endif /* HAX_OPENAI_H */
