/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_RESPONSES_MESSAGES_H
#define HAX_PROVIDERS_RESPONSES_MESSAGES_H

#include <jansson.h>

#include "provider.h"

/* Translate transcript items into a newly allocated Responses API input array. Encrypted reasoning
 * is replayed only when its provider/model stamp matches the current request. Tool-result images
 * become input_image parts when `image_input` is nonzero, or text placeholders when it is zero. The
 * caller must json_decref the returned array. */
json_t *responses_build_input_items(const struct item *items, size_t n_items, const char *provider,
                                    const char *model, int image_input);

/* Build the newly allocated Responses API tool array; function schemas are flat, not nested under
 * a "function" object as in Chat Completions. The caller must json_decref it. */
json_t *responses_build_tools(const struct tool_def *tools, size_t n_tools);

/* Assemble the protocol-level Responses request: model, stream, store, instructions, input, tools,
 * and the reasoning block. Reasoning is requested with encrypted content so the model can carry a
 * chain of thought across the tool calls of one turn, which the backend returns only for an
 * unstored response. Callers add their own auth-, routing-, and vendor-specific fields, and must
 * json_decref the result. */
json_t *responses_build_body(const struct context *context, const char *provider,
                             const char *model);

#endif /* HAX_PROVIDERS_RESPONSES_MESSAGES_H */
