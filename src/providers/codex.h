/* SPDX-License-Identifier: MIT */
#ifndef HAX_CODEX_H
#define HAX_CODEX_H

#include <jansson.h>

#include "provider.h"

/* Reads ~/.codex/auth.json (tokens.access_token + tokens.account_id).
 * Returns NULL on failure (prints cause to stderr). */
struct provider *codex_provider_new(const char *name);

/* Diagnostic for a failed codex model-catalog fetch, keyed by HTTP status
 * (0 = never reached). Exposed for unit tests. Heap-owned; caller frees. */
char *codex_models_error(long status);

/* Translate flat conversation items into the Responses API `input` array.
 * Exposed for unit testing the round-trip without an HTTP call: user/
 * assistant messages, function_call / function_call_output, reasoning-blob
 * replay (gated on the provenance stamp matching provider/model), and
 * tool-result image parts (input_image data URLs, or input_text
 * placeholders when image_input is 0). Returns a new jansson array; caller
 * json_decref. */
json_t *codex_build_input_items(const struct item *items, size_t n, const char *provider,
                                const char *model, int image_input);

extern const struct provider_factory PROVIDER_CODEX;

#endif /* HAX_CODEX_H */
