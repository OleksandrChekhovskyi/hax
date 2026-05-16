/* SPDX-License-Identifier: MIT */
#ifndef HAX_OLLAMA_H
#define HAX_OLLAMA_H

#include "provider.h"

/* Ollama preset over its OpenAI-compatible Chat Completions surface. Reads:
 *   HAX_MODEL            — required; ollama's chat endpoint rejects an
 *                          empty model field, and no signal it exposes
 *                          (/api/ps decays after OLLAMA_KEEP_ALIVE,
 *                          /v1/models is just the pulled-model catalog
 *                          with no preference order) reliably picks the
 *                          model the user wants — so we ask once rather
 *                          than guess
 *   HAX_OLLAMA_PORT      — optional port (defaults to 11434); only used
 *                          when HAX_OPENAI_BASE_URL is unset
 *   HAX_OPENAI_BASE_URL  — full override (e.g. for a non-localhost server)
 *   HAX_OPENAI_API_KEY   — optional; ollama doesn't authenticate by
 *                          default but a reverse proxy in front of it
 *                          might, so the token is forwarded both to chat
 *                          and the /api/show probe
 *
 * On startup, after HAX_MODEL is validated, asynchronously probes:
 *   POST /api/show {"model": "<id>"} →  fills provider->context_limit
 *                                       from model_info["<arch>.context_length"]
 *
 * Probe behavior:
 *   - /api/show reports the model's training context length (the gguf
 *     `<arch>.context_length` KV). When the ollama daemon is configured
 *     with OLLAMA_CONTEXT_LENGTH (or a per-request num_ctx) smaller than
 *     this value, the percentage display under-reports — set
 *     HAX_CONTEXT_LIMIT to override.
 *   - The probe is always best-effort: failure just leaves the
 *     percentage display hidden, never blocks startup.
 *   - HAX_CONTEXT_LIMIT (when set) bypasses the probe entirely and is
 *     used verbatim by the agent. */
struct provider *ollama_provider_new(void);

extern const struct provider_factory PROVIDER_OLLAMA;

#endif /* HAX_OLLAMA_H */
