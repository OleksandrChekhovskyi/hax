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
 *   GET /v1/models  →  reconciles the model against what the server
 *                      actually serves (synchronous — the model is needed
 *                      before the first chat request): a configured model
 *                      present in the list is kept; unset or absent, the
 *                      first served entry is adopted as a session override
 *                      (absent = with a warning — see
 *                      llamacpp_reconcile_model)
 *   GET /props      →  fills provider->context_limit from
 *                      default_generation_settings.n_ctx (asynchronous —
 *                      runs in the background so a slow /props doesn't
 *                      delay the first prompt; the percentage display
 *                      just lights up once the response lands)
 *
 * Probe behavior:
 *   - When no model is configured and the /v1/models probe fails,
 *     construction fails loudly with the URL it tried — that's a strong
 *     signal the server is unreachable, and surfacing it here saves the
 *     user from a misleading downstream "HAX_MODEL is required" message.
 *   - When a model is configured and the server is unreachable, it is
 *     trusted as-is: the first stream surfaces the real connection error
 *     instead of construction failing.
 *   - The /props probe is always best-effort: failure just leaves the
 *     percentage display hidden, never blocks startup.
 *   - HAX_CONTEXT_LIMIT (when set) bypasses the /props probe entirely
 *     and is used verbatim by the agent. */
struct provider *llamacpp_provider_new(const char *name);

/* The pure model-reconcile decision behind the /v1/models probe, exposed
 * for tests. Returns 0 when `body` names at least one served model, with
 * *adopt set to a malloc'd replacement when `cur` is unset/absent from the
 * list (NULL when `cur` is served and kept); -1 for an unusable body or an
 * empty list. */
int llamacpp_reconcile_model(const char *body, const char *cur, char **adopt);

/* Collapse a GGUF path to its extensionless filename for display and the env
 * block. Other model ids are duplicated unchanged. Caller frees. */
char *llamacpp_model_label(struct provider *p, const char *model);

extern const struct provider_factory PROVIDER_LLAMACPP;

#endif /* HAX_LLAMACPP_H */
