/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CODEX_MESSAGES_H
#define HAX_PROVIDERS_CODEX_MESSAGES_H

#include <jansson.h>

#include "provider.h"

/* Translate transcript items into a newly allocated Responses API input array. Encrypted reasoning
 * is replayed only when its provider/model stamp matches the current request. Tool-result images
 * become input_image parts when `image_input` is nonzero, or text placeholders when it is zero. The
 * caller must json_decref the returned array. */
json_t *codex_build_input_items(const struct item *items, size_t n_items, const char *provider,
                                const char *model, int image_input);

#endif /* HAX_PROVIDERS_CODEX_MESSAGES_H */
