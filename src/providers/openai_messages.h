/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_OPENAI_MESSAGES_H
#define HAX_PROVIDERS_OPENAI_MESSAGES_H

#include <jansson.h>

#include "provider.h"

enum openai_reasoning_format {
    OPENAI_REASONING_FLAT = 0, /* reasoning_effort: <effort> */
    OPENAI_REASONING_NESTED,   /* reasoning: {enabled: bool, effort?: <effort>} */
};

/* NULL, empty, and unknown values return `fallback`; unknown values also warn. */
enum openai_reasoning_format openai_reasoning_format_parse(const char *value,
                                                           enum openai_reasoning_format fallback);

/* Add the selected reasoning representation to `body`; NULL or empty effort is omitted. */
void openai_apply_reasoning(json_t *body, enum openai_reasoning_format format, const char *effort);

/* Translate transcript items into a newly allocated Chat Completions messages array.
 * Reasoning is replayed only when `reasoning_field` is set and the item's provider/model stamp
 * matches `current_provider` and `current_model`. Tool-result images become a following user
 * message when `image_input` is nonzero, or text placeholders when it is zero. The caller owns the
 * returned array and must json_decref it. */
json_t *openai_build_messages(const char *system_prompt, const struct item *items, size_t n_items,
                              const char *reasoning_field, const char *current_provider,
                              const char *current_model, int image_input);

/* Mark the system prompt and conversation tail with cache_control breakpoints. A "1h" ttl is
 * encoded explicitly; other values leave the backend default. */
void openai_apply_cache_breakpoints(json_t *messages, const char *ttl);

#endif /* HAX_PROVIDERS_OPENAI_MESSAGES_H */
