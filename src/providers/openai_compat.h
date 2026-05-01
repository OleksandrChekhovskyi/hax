/* SPDX-License-Identifier: MIT */
#ifndef HAX_OPENAI_COMPAT_H
#define HAX_OPENAI_COMPAT_H

#include "provider.h"

/* Generic "OpenAI-compatible" preset for any third-party endpoint that
 * speaks /v1/chat/completions: vLLM, Ollama, LM Studio, oMLX, custom
 * proxies, hosted compat backends without a dedicated preset. Reads:
 *   HAX_OPENAI_BASE_URL  — REQUIRED. The full URL of the target's /v1
 *                          (e.g. http://127.0.0.1:8000/v1)
 *   HAX_OPENAI_API_KEY   — optional Bearer token; many local servers
 *                          run unauthenticated
 *   HAX_PROVIDER_NAME    — optional display name (defaults to
 *                          "openai-compatible")
 *
 * Deliberately does NOT fall back to OPENAI_API_KEY — that key is scoped
 * to the dedicated openai preset, so a globally configured key never
 * leaks to a third-party endpoint. prompt_cache_key is off by default
 * (some backends like vLLM reject unknown JSON fields); set
 * HAX_OPENAI_SEND_CACHE_KEY to opt in for backends that honor it. */
struct provider *openai_compat_provider_new(void);

extern const struct provider_factory PROVIDER_OPENAI_COMPAT;

#endif /* HAX_OPENAI_COMPAT_H */
