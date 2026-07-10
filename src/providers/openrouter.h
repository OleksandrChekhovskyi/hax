/* SPDX-License-Identifier: MIT */
#ifndef HAX_OPENROUTER_H
#define HAX_OPENROUTER_H

#include "provider.h"

/* OpenRouter preset over the OpenAI-compatible Chat Completions API. Locked
 * to https://openrouter.ai/api/v1 — HAX_OPENAI_BASE_URL is rejected so the
 * OPENROUTER_API_KEY fallback and the attribution headers can never be sent
 * to an unrelated host. For custom OpenAI-compatible endpoints, use the
 * openai-compatible preset (openai_compat.h) instead.
 *
 * Reads config from env:
 *   HAX_OPENAI_API_KEY       — preferred; falls back to OPENROUTER_API_KEY
 *   HAX_OPENROUTER_REFERER   — HTTP-Referer, OpenRouter's app identifier for
 *                              attribution; defaults to the project URL, an
 *                              empty value disables attribution
 *   HAX_OPENROUTER_TITLE     — X-Title, the display name for the app page
 *                              (defaults to "hax"; empty omits it)
 *
 * prompt_cache_key is sent by default since OpenRouter routes some upstream
 * providers (Anthropic, OpenAI) that honor prefix-cache hints. */
struct provider *openrouter_provider_new(const char *name);

extern const struct provider_factory PROVIDER_OPENROUTER;

#endif /* HAX_OPENROUTER_H */
