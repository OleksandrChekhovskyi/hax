/* SPDX-License-Identifier: MIT */
#include "providers/wire.h"

#include <jansson.h>
#include <stddef.h>
#include <strings.h>

#include "provider.h"
#include "providers/anthropic_body.h"
#include "providers/anthropic_events.h"
#include "providers/chat_body.h"
#include "providers/chat_events.h"
#include "providers/config_provider.h"
#include "providers/responses_body.h"
#include "providers/responses_events.h"

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
    chat_events_init(&events->chat, callback, callback_user);
    if (opts) {
        events->chat.emit_progress = opts->emit_progress;
        events->chat.length_hint = opts->length_hint;
        events->chat.cache_write_1h = opts->cache_write_1h;
    }
}

static void chat_feed(union wire_events *events, const char *event_name, const char *data)
{
    (void)event_name;
    chat_events_feed(&events->chat, data);
}

static void chat_finalize(union wire_events *events)
{
    chat_events_finalize(&events->chat);
}

static void chat_free(union wire_events *events)
{
    chat_events_free(&events->chat);
}

static int chat_complete(const union wire_events *events)
{
    return chat_events_complete(&events->chat);
}

static const struct stream_usage *chat_usage(const union wire_events *events)
{
    return &events->chat.usage;
}

const struct wire WIRE_OPENAI_CHAT = {
    .id = "openai-completions",
    .path = "/chat/completions",
    .build_body = chat_build_body,
    .events_init = chat_init,
    .events_feed = chat_feed,
    .events_finalize = chat_finalize,
    .events_free = chat_free,
    .events_complete = chat_complete,
    .events_usage = chat_usage,
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

static int responses_complete(const union wire_events *events)
{
    return events->responses.terminal_emitted;
}

const struct wire WIRE_OPENAI_RESPONSES = {
    .id = "openai-responses",
    .path = "/responses",
    .build_body = responses_build_body,
    .events_init = responses_init,
    .events_feed = responses_feed,
    .events_finalize = responses_finalize,
    .events_free = responses_free,
    .events_complete = responses_complete,
};

static void anthropic_init(union wire_events *events, stream_cb callback, void *callback_user,
                           const struct wire_events_opts *opts)
{
    (void)opts;
    anthropic_events_init(&events->anthropic, callback, callback_user);
}

static void anthropic_feed(union wire_events *events, const char *event_name, const char *data)
{
    anthropic_events_feed(&events->anthropic, event_name, data);
}

static void anthropic_finalize(union wire_events *events)
{
    anthropic_events_finalize(&events->anthropic);
}

static void anthropic_free(union wire_events *events)
{
    anthropic_events_free(&events->anthropic);
}

static int anthropic_complete(const union wire_events *events)
{
    return events->anthropic.terminal_emitted;
}

static const struct stream_usage *anthropic_usage(const union wire_events *events)
{
    return &events->anthropic.usage;
}

const struct wire WIRE_ANTHROPIC_MESSAGES = {
    .id = "anthropic-messages",
    .path = "/messages",
    .build_body = anthropic_build_body,
    .events_init = anthropic_init,
    .events_feed = anthropic_feed,
    .events_finalize = anthropic_finalize,
    .events_free = anthropic_free,
    .events_complete = anthropic_complete,
    .events_usage = anthropic_usage,
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
