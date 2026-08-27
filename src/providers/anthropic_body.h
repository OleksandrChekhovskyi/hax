/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_ANTHROPIC_BODY_H
#define HAX_PROVIDERS_ANTHROPIC_BODY_H

#include <jansson.h>

#include "provider.h"

/* Request-body construction for the Anthropic Messages dialect. */

/* Adaptive thinking uses output_config.effort; budget thinking uses a token count. */
enum anthropic_thinking_mode {
    ANTHROPIC_THINKING_ADAPTIVE = 0,
    ANTHROPIC_THINKING_BUDGET,
    ANTHROPIC_THINKING_OFF,
};

/* Parse a thinking-mode name case-insensitively. Returns the mode, or -1 for NULL or an
 * unrecognized value, so callers choose their own fallback and diagnostics. */
int anthropic_thinking_mode_parse(const char *value);

/* Whether output_config can carry `effort`: the Messages wire vocabulary starts at low, so
 * "none" and "minimal" have no spelling. Requests express "none" as thinking off and clamp
 * "minimal" to the low floor. */
int anthropic_effort_expressible(const char *effort);

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

#endif /* HAX_PROVIDERS_ANTHROPIC_BODY_H */
