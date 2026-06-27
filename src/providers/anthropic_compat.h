/* SPDX-License-Identifier: MIT */
#ifndef HAX_ANTHROPIC_COMPAT_H
#define HAX_ANTHROPIC_COMPAT_H

#include "provider.h"

/* Generic "anthropic-compatible" preset for any endpoint that speaks the
 * Messages API at /v1/messages: a local llama-server, z.ai, or any proxy
 * emulating Anthropic. Reads:
 *   HAX_ANTHROPIC_BASE_URL  — REQUIRED. The /v1 root (e.g.
 *                             http://127.0.0.1:18080/v1)
 *   HAX_ANTHROPIC_API_KEY   — optional x-api-key token; local servers often
 *                             run unauthenticated
 *   HAX_PROVIDER_NAME       — optional display name
 *
 * Deliberately does NOT fall back to ANTHROPIC_API_KEY — that key is scoped to
 * the dedicated anthropic preset. Defaults to budget-mode thinking (the legacy
 * thinking parameter local servers accept), tolerates empty thinking
 * signatures, and leaves prompt caching off (opt in with HAX_ANTHROPIC_CACHE).
 * Like the openai-compatible shim, it does not probe for a model at startup —
 * set HAX_MODEL or pick one with /model. */
struct provider *anthropic_compat_provider_new(const char *name);

extern const struct provider_factory PROVIDER_ANTHROPIC_COMPAT;

#endif /* HAX_ANTHROPIC_COMPAT_H */
