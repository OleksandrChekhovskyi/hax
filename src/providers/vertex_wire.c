/* SPDX-License-Identifier: MIT */
#include "providers/vertex_wire.h"

#include <jansson.h>

#include "providers/anthropic_body.h"
#include "providers/anthropic_events.h"

/* The endpoint answers the dialect's version from the body, and refuses a body that names the
 * model: the model belongs in the path. */
#define VERTEX_ANTHROPIC_VERSION "vertex-2023-10-16"

/* The Messages body without its model member, plus the version the endpoint asks for. */
static json_t *vertex_anthropic_build_body(const struct context *context, const char *provider_id,
                                           const char *model, const struct wire_body_opts *opts)
{
    json_t *body = anthropic_build_body(context, provider_id, model, opts);
    if (!body)
        return NULL;
    json_object_del(body, "model");
    json_object_set_new(body, "anthropic_version", json_string(VERTEX_ANTHROPIC_VERSION));
    return body;
}

static void vertex_anthropic_init(union wire_events *events, stream_cb callback,
                                  void *callback_user, const struct wire_events_opts *opts)
{
    (void)opts;
    anthropic_events_init(&events->anthropic, callback, callback_user);
}

static void vertex_anthropic_feed(union wire_events *events, const char *event_name,
                                  const char *data)
{
    anthropic_events_feed(&events->anthropic, event_name, data);
}

static void vertex_anthropic_finalize(union wire_events *events)
{
    anthropic_events_finalize(&events->anthropic);
}

static void vertex_anthropic_free(union wire_events *events)
{
    anthropic_events_free(&events->anthropic);
}

static int vertex_anthropic_complete(const union wire_events *events)
{
    return events->anthropic.terminal_emitted;
}

static const struct stream_usage *vertex_anthropic_usage(const union wire_events *events)
{
    return &events->anthropic.usage;
}

const struct wire WIRE_ANTHROPIC_MESSAGES_VERTEX = {
    .id = "anthropic-messages-vertex",
    .path = ":streamRawPredict",
    .messages = 1,
    .build_body = vertex_anthropic_build_body,
    .events_init = vertex_anthropic_init,
    .events_feed = vertex_anthropic_feed,
    .events_finalize = vertex_anthropic_finalize,
    .events_free = vertex_anthropic_free,
    .events_complete = vertex_anthropic_complete,
    .events_usage = vertex_anthropic_usage,
};
