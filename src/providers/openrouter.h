/* SPDX-License-Identifier: MIT */
#ifndef HAX_OPENROUTER_H
#define HAX_OPENROUTER_H

#include <jansson.h>

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

/* Fill `out` from one OpenRouter `/models` `data[]` element: context window,
 * image input, tool support, per-Mtok rates (input, cached input, output),
 * and the description's lead line. Wired in as the preset's
 * parse_model hook (openai.h), so `out` arrives model_info_init'd with `id`
 * already set and every field this can't read stays at its unknown
 * sentinel. Pure — exposed separately from the HTTP path so the catalog
 * shape can be unit-tested against a fixture. */
void openrouter_parse_model(const json_t *entry, struct model_info *out);

extern const struct provider_factory PROVIDER_OPENROUTER;

#endif /* HAX_OPENROUTER_H */
