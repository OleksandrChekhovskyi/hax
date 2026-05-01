/* SPDX-License-Identifier: MIT */
#ifndef HAX_OPENROUTER_H
#define HAX_OPENROUTER_H

#include "provider.h"

/* OpenRouter preset over the OpenAI-compatible Chat Completions API. Locked
 * to https://openrouter.ai/api/v1 — HAX_OPENAI_BASE_URL is rejected so the
 * OPENROUTER_API_KEY fallback and the X-Title attribution header can never
 * be sent to an unrelated host. For custom OpenAI-compatible endpoints, use
 * the openai-compatible preset (openai_compat.h) instead.
 *
 * Reads config from env:
 *   HAX_OPENAI_API_KEY       — preferred; falls back to OPENROUTER_API_KEY
 *   HAX_OPENROUTER_TITLE     — optional X-Title (defaults to "hax"); used by
 *                              OpenRouter to attribute usage on its dashboards
 *   HAX_OPENROUTER_REFERER   — optional HTTP-Referer; omitted when unset
 *
 * prompt_cache_key is sent by default since OpenRouter routes some upstream
 * providers (Anthropic, OpenAI) that honor prefix-cache hints. */
struct provider *openrouter_provider_new(void);

extern const struct provider_factory PROVIDER_OPENROUTER;

#endif /* HAX_OPENROUTER_H */
