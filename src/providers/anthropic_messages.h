/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_ANTHROPIC_MESSAGES_H
#define HAX_PROVIDERS_ANTHROPIC_MESSAGES_H

#include <jansson.h>

#include "provider.h"

/* Translate transcript items into a newly allocated Messages API array. Opaque reasoning is
 * replayed only when its provider/model stamp matches the current request. Empty thinking
 * signatures become text unless `allow_empty_signature` is set. Tool-result images become image
 * blocks when `image_input` is nonzero, or text placeholders when it is zero. The caller must
 * json_decref the returned array. */
json_t *anthropic_build_messages(const struct item *items, size_t n_items,
                                 const char *current_provider, const char *current_model,
                                 int allow_empty_signature, int image_input);

#endif /* HAX_PROVIDERS_ANTHROPIC_MESSAGES_H */
