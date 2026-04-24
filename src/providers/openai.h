/* SPDX-License-Identifier: MIT */
#ifndef HAX_OPENAI_H
#define HAX_OPENAI_H

#include "provider.h"

/* Generic OpenAI-compatible Chat Completions provider. Reads config from
 * env:
 *   HAX_OPENAI_BASE_URL  — optional, defaults to "https://api.openai.com/v1";
 *                          set to point at a local or proxy endpoint
 *   HAX_OPENAI_API_KEY   — optional; when set, sent as `Authorization: Bearer
 *                          <key>`. Falls back to OPENAI_API_KEY only when
 *                          HAX_OPENAI_BASE_URL is unset, to avoid leaking a
 *                          globally configured key to custom endpoints.
 *   HAX_PROVIDER_NAME    — optional display name (defaults to "openai")
 * Returns NULL on failure (prints cause to stderr). */
struct provider *openai_provider_new(void);

#endif /* HAX_OPENAI_H */
