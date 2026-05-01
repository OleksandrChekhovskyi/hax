/* SPDX-License-Identifier: MIT */
#ifndef HAX_LLAMACPP_H
#define HAX_LLAMACPP_H

#include "provider.h"

/* llama.cpp llama-server preset over the OpenAI-compatible Chat Completions
 * API. Reads config from env:
 *   HAX_LLAMACPP_PORT    — optional port (defaults to 8080); only used when
 *                          HAX_OPENAI_BASE_URL is unset
 *   HAX_OPENAI_BASE_URL  — full override (e.g. for a non-localhost server)
 *   HAX_OPENAI_API_KEY   — optional; most local llama-server runs leave
 *                          authentication off, but if --api-key is in use
 *                          the token is forwarded to the discovery probes
 *                          too (otherwise /v1/models 401s and construction
 *                          fails before chat would have been authorized)
 *
 * On startup, probes the resolved server:
 *   GET /v1/models  →  fills HAX_MODEL from data[0].id when unset
 *   GET /props      →  fills HAX_CONTEXT_LIMIT from
 *                      default_generation_settings.n_ctx when unset
 *
 * Probe behavior:
 *   - When HAX_MODEL is unset and the /v1/models probe fails, construction
 *     fails loudly with the URL it tried — that's a strong signal the
 *     server is unreachable, and surfacing it here saves the user from a
 *     misleading downstream "HAX_MODEL is required" message.
 *   - When HAX_MODEL is set, the probe is skipped entirely.
 *   - The /props probe is always best-effort: failure just leaves the
 *     percentage display hidden, never blocks startup. */
struct provider *llamacpp_provider_new(void);

extern const struct provider_factory PROVIDER_LLAMACPP;

#endif /* HAX_LLAMACPP_H */
