/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_WIRE_H
#define HAX_PROVIDERS_WIRE_H

#include <jansson.h>

#include "provider.h"
#include "providers/anthropic_body.h"
#include "providers/anthropic_events.h"
#include "providers/chat_body.h"
#include "providers/chat_events.h"
#include "providers/responses_events.h"

/* A wire dialect: the request path a protocol serves under a provider's base URL, the request
 * body serializer, and the translator from its SSE payloads to provider-independent stream
 * events. Adapters resolve provider policy (config, model metadata, credentials) and hand the
 * results in as options, so wires stay pure protocol. */

/* Parser state for one streaming response; the selected wire defines the active member. */
union wire_events {
    struct chat_events chat;
    struct responses_events responses;
    struct anthropic_events anthropic;
};

/* Per-stream translator knobs. Wires without a matching field ignore them. */
struct wire_events_opts {
    const char *length_hint; /* chat: borrowed; appended to "length" errors */
    int cache_write_1h;      /* chat: bill cache writes at the 1-hour rate */
};

/* Per-request body options. Wires read only the fields their dialect defines. */
struct wire_body_opts {
    const json_t *extra_body; /* user passthrough, merged over the finished body */
    const char *temperature;
    const char *top_p;
    /* chat + anthropic: send explicit cache markers with this ttl */
    int cache_markers;
    const char *cache_ttl;
    /* chat + responses: prompt_cache_key; NULL omits it */
    const char *session_cache_key;
    /* chat */
    const char *reasoning_field; /* replay reasoning under this member; NULL disables */
    enum chat_reasoning_format reasoning_format;
    int request_cost; /* request OpenRouter usage cost */
    /* anthropic */
    int max_tokens;
    enum anthropic_thinking_mode thinking_mode;
    int thinking_budget;       /* tokens; out-of-range values fall back to max_tokens - 1 */
    int show_reasoning;        /* adaptive thinking: summarized display instead of omitted */
    int allow_empty_signature; /* preserve unsigned thinking blocks on compat backends */
};

struct wire {
    const char *id;   /* canonical dialect name accepted by `api` config fields */
    const char *path; /* request path appended to the provider base URL */
    /* Compose one request minus the extra_body passthrough; wire_build_body finishes it. */
    json_t *(*build_body)(const struct context *context, const char *provider_id, const char *model,
                          const struct wire_body_opts *opts);
    /* `opts` may be NULL. Free with events_free; a finalized parser still needs freeing. */
    void (*events_init)(union wire_events *events, stream_cb callback, void *callback_user,
                        const struct wire_events_opts *opts);
    /* Feed one SSE payload; `event_name` may be NULL. */
    void (*events_feed)(union wire_events *events, const char *event_name, const char *data);
    /* Finish a cleanly closed transport; emits an error if no terminal state was received. */
    void (*events_finalize)(union wire_events *events);
    void (*events_free)(union wire_events *events);
    /* Non-zero once a terminal state was received; a transport that closes without one died
     * mid-stream. */
    int (*events_complete)(const union wire_events *events);
    /* Borrowed usage captured so far, or NULL when the dialect only reports usage with its
     * terminal event. */
    const struct stream_usage *(*events_usage)(const union wire_events *events);
};

extern const struct wire WIRE_OPENAI_CHAT;      /* OpenAI Chat Completions */
extern const struct wire WIRE_OPENAI_RESPONSES; /* OpenAI Responses */
extern const struct wire WIRE_ANTHROPIC_MESSAGES;

/* Resolve an `api` dialect name — canonical spellings plus the short "chat" and "responses" —
 * to its wire, case-insensitively. NULL and unknown names return NULL. */
const struct wire *wire_find(const char *api);

/* Serialize one request: the wire's body plus the extra_body passthrough, merged last so it can
 * override protocol defaults. Returns owned compact JSON; the caller frees it. */
char *wire_build_body(const struct wire *wire, const struct context *context,
                      const char *provider_id, const char *model,
                      const struct wire_body_opts *opts);

#endif /* HAX_PROVIDERS_WIRE_H */
