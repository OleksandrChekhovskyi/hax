/* SPDX-License-Identifier: MIT */
#include "providers/wire.h"

#include <jansson.h>
#include <stddef.h>
#include <strings.h>

#include "provider.h"
#include "providers/anthropic_events.h"
#include "providers/anthropic_messages.h"
#include "providers/config_provider.h"
#include "providers/openai_events.h"
#include "providers/openai_messages.h"
#include "providers/responses_events.h"
#include "providers/responses_messages.h"

char *wire_build_body(const struct wire *wire, const struct context *context,
                      const char *provider_id, const char *model, const struct wire_body_opts *opts)
{
    json_t *body = wire->build_body(context, provider_id, model, opts);
    provider_extra_body_apply(body, opts->extra_body);
    char *json = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    return json;
}

static void chat_init(union wire_events *events, stream_cb callback, void *callback_user,
                      const struct wire_events_opts *opts)
{
    openai_events_init(&events->chat, callback, callback_user);
    if (opts) {
        events->chat.emit_progress = opts->emit_progress;
        events->chat.length_hint = opts->length_hint;
        events->chat.cache_write_1h = opts->cache_write_1h;
    }
}

static void chat_feed(union wire_events *events, const char *event_name, const char *data)
{
    (void)event_name;
    openai_events_feed(&events->chat, data);
}

static void chat_finalize(union wire_events *events)
{
    openai_events_finalize(&events->chat);
}

static void chat_free(union wire_events *events)
{
    openai_events_free(&events->chat);
}

const struct wire WIRE_OPENAI_CHAT = {
    .id = "openai-completions",
    .path = "/chat/completions",
    .build_body = openai_build_body,
    .events_init = chat_init,
    .events_feed = chat_feed,
    .events_finalize = chat_finalize,
    .events_free = chat_free,
};

static void responses_init(union wire_events *events, stream_cb callback, void *callback_user,
                           const struct wire_events_opts *opts)
{
    (void)opts;
    responses_events_init(&events->responses, callback, callback_user);
}

static void responses_feed(union wire_events *events, const char *event_name, const char *data)
{
    (void)event_name;
    responses_events_feed(&events->responses, data);
}

static void responses_finalize(union wire_events *events)
{
    responses_events_finalize(&events->responses);
}

static void responses_free(union wire_events *events)
{
    responses_events_free(&events->responses);
}

const struct wire WIRE_OPENAI_RESPONSES = {
    .id = "openai-responses",
    .path = "/responses",
    .build_body = responses_build_body,
    .events_init = responses_init,
    .events_feed = responses_feed,
    .events_finalize = responses_finalize,
    .events_free = responses_free,
};

static void messages_init(union wire_events *events, stream_cb callback, void *callback_user,
                          const struct wire_events_opts *opts)
{
    (void)opts;
    anthropic_events_init(&events->messages, callback, callback_user);
}

static void messages_feed(union wire_events *events, const char *event_name, const char *data)
{
    anthropic_events_feed(&events->messages, event_name, data);
}

static void messages_finalize(union wire_events *events)
{
    anthropic_events_finalize(&events->messages);
}

static void messages_free(union wire_events *events)
{
    anthropic_events_free(&events->messages);
}

const struct wire WIRE_ANTHROPIC_MESSAGES = {
    .id = "anthropic-messages",
    .path = "/messages",
    .build_body = anthropic_build_body,
    .events_init = messages_init,
    .events_feed = messages_feed,
    .events_finalize = messages_finalize,
    .events_free = messages_free,
};

const struct wire *wire_find(const char *api)
{
    static const struct wire *const WIRES[] = {&WIRE_OPENAI_CHAT, &WIRE_OPENAI_RESPONSES,
                                               &WIRE_ANTHROPIC_MESSAGES};
    if (!api)
        return NULL;
    for (size_t i = 0; i < sizeof(WIRES) / sizeof(WIRES[0]); i++)
        if (strcasecmp(api, WIRES[i]->id) == 0)
            return WIRES[i];
    /* The short spellings HAX_OPENAI_API documents. */
    if (strcasecmp(api, "chat") == 0)
        return &WIRE_OPENAI_CHAT;
    if (strcasecmp(api, "responses") == 0)
        return &WIRE_OPENAI_RESPONSES;
    return NULL;
}
