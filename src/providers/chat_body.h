/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CHAT_BODY_H
#define HAX_PROVIDERS_CHAT_BODY_H

#include <jansson.h>

#include "provider.h"

/* Request-body construction for the OpenAI Chat Completions dialect, and the reasoning and
 * cache-marker request shapes that vary across its compatible backends. */

enum chat_reasoning_format {
    CHAT_REASONING_FLAT = 0, /* reasoning_effort: <effort> */
    CHAT_REASONING_NESTED,   /* reasoning: {enabled: bool, effort?: <effort>} */
};

enum chat_cache_mode {
    CHAT_CACHE_OFF,
    CHAT_CACHE_AUTO,
    CHAT_CACHE_ON,
};

struct chat_cache_plan {
    int send_breakpoints;
    int writes_bill_1h;
};

struct catalog_entry; /* catalog.h */

/* Derive Chat Completions cache-marker and billing behavior from the model's cache rates. */
struct chat_cache_plan chat_plan_cache(const struct catalog_entry *rates, enum chat_cache_mode mode,
                                       const char *ttl);

/* NULL, empty, and unknown values return `fallback`; unknown values also warn. */
enum chat_reasoning_format chat_reasoning_format_parse(const char *value,
                                                       enum chat_reasoning_format fallback);

/* Add the selected reasoning representation to `body`; NULL or empty effort is omitted. */
void chat_apply_reasoning(json_t *body, enum chat_reasoning_format format, const char *effort);

/* Translate transcript items into a newly allocated Chat Completions messages array.
 * Reasoning is replayed only when `reasoning_field` is set and the item's provider/model stamp
 * matches `current_provider` and `current_model`. Tool-result images become a following user
 * message when `image_input` is nonzero, or text placeholders when it is zero. The caller owns the
 * returned array and must json_decref it. */
json_t *chat_build_messages(const char *system_prompt, const struct item *items, size_t n_items,
                            const char *reasoning_field, const char *current_provider,
                            const char *current_model, int image_input);

/* Mark the system prompt and conversation tail with cache_control breakpoints. A "1h" ttl is
 * encoded explicitly; other values leave the backend default. */
void chat_apply_cache_breakpoints(json_t *messages, const char *ttl);

struct wire_body_opts; /* wire.h */

/* Assemble the full Chat Completions request except the extra_body passthrough, which the wire
 * layer merges last. The caller must json_decref the result. */
json_t *chat_build_body(const struct context *context, const char *provider_id, const char *model,
                        const struct wire_body_opts *opts);

#endif /* HAX_PROVIDERS_CHAT_BODY_H */
