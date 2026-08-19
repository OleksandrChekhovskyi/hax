/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_ANTHROPIC_MESSAGES_H
#define HAX_PROVIDERS_ANTHROPIC_MESSAGES_H

#include <jansson.h>

#include "provider.h"

/* Adaptive thinking uses output_config.effort; budget thinking uses a token count. */
enum anthropic_thinking_mode {
    ANTHROPIC_THINKING_ADAPTIVE = 0,
    ANTHROPIC_THINKING_BUDGET,
    ANTHROPIC_THINKING_OFF,
};

/* Translate transcript items into a newly allocated Messages API array. Opaque reasoning is
 * replayed only when its provider/model stamp matches the current request. Empty thinking
 * signatures become text unless `allow_empty_signature` is set. Tool-result images become image
 * blocks when `image_input` is nonzero, or text placeholders when it is zero. The caller must
 * json_decref the returned array. */
json_t *anthropic_build_messages(const struct item *items, size_t n_items,
                                 const char *current_provider, const char *current_model,
                                 int allow_empty_signature, int image_input);

struct wire_body_opts; /* wire.h */

/* Assemble the full Messages API request except the extra_body passthrough, which the wire
 * layer merges last. The caller must json_decref the result. */
json_t *anthropic_build_body(const struct context *context, const char *provider_id,
                             const char *model, const struct wire_body_opts *opts);

#endif /* HAX_PROVIDERS_ANTHROPIC_MESSAGES_H */
