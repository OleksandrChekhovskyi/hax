/* SPDX-License-Identifier: MIT */
#include "agent_stream.h"

#include <stdlib.h>
#include <string.h>

#include "agent_usage.h"
#include "provider.h"
#include "turn.h"
#include "xalloc.h"
#include "transport/api_error.h"

static const struct stream_usage UNREPORTED_USAGE = {-1, -1, -1, -1, -1, -1};

struct stream_sink {
    struct agent_stream *stream;
    stream_cb observer;
    void *observer_user;
    agent_signal_cb poll;
    void *poll_user;
};

void agent_stream_init(struct agent_stream *stream)
{
    memset(stream, 0, sizeof(*stream));
    turn_init(&stream->assembly);
    stream->usage = UNREPORTED_USAGE;
    attempt_log_init(&stream->attempts);
}

void agent_stream_destroy(struct agent_stream *stream)
{
    turn_reset(&stream->assembly);
    free(stream->error_message);
    stream->error_message = NULL;
    attempt_log_free(&stream->attempts);
}

static int sink_on_event(const struct stream_event *event, void *user)
{
    struct stream_sink *sink = user;
    struct agent_stream *stream = sink->stream;

    if (event->kind == EV_DONE) {
        stream->usage = event->u.done.usage;
        attempt_log_record(&stream->attempts, &event->u.done.usage, &event->u.done.response);
    } else if (event->kind == EV_ERROR) {
        if (!stream->error_message && event->u.error.message) {
            stream->error_message = xstrdup(event->u.error.message);
            stream->error_kind = event->u.error.kind;
        }
        if (event->u.error.usage) {
            stream->usage = *event->u.error.usage;
            attempt_log_record(&stream->attempts, event->u.error.usage, event->u.error.response);
        }
    } else if (event->kind == EV_RETRY) {
        attempt_log_retry(&stream->attempts, event->u.retry.usage, event->u.retry.delay_ms);
    }

    if (sink->observer)
        sink->observer(event, sink->observer_user);
    turn_consume(&stream->assembly, event);
    return 0;
}

static int sink_tick(void *user)
{
    struct stream_sink *sink = user;
    return agent_signal_stops(sink->poll(sink->poll_user), &sink->stream->assembly);
}

void agent_stream_request(struct agent_stream *stream, struct provider *provider,
                          const struct context *context, const char *model, stream_cb observer,
                          void *observer_user, agent_signal_cb poll, void *poll_user)
{
    turn_reset(&stream->assembly);
    stream->usage = UNREPORTED_USAGE;
    free(stream->error_message);
    stream->error_message = NULL;
    stream->error_kind = API_ERROR_OTHER;
    struct stream_sink sink = {
        .stream = stream,
        .observer = observer,
        .observer_user = observer_user,
        .poll = poll,
        .poll_user = poll_user,
    };
    provider->stream(provider, context, model, sink_on_event, &sink, poll ? sink_tick : NULL,
                     &sink);
}

int agent_signal_stops(enum agent_signal signal, const struct turn *assembly)
{
    if (signal == AGENT_SIGNAL_ABORT)
        return 1;
    return signal == AGENT_SIGNAL_PAUSE && !turn_has_content(assembly);
}

int agent_stream_cut_by(const struct agent_stream *stream, enum agent_signal signal)
{
    return stream->assembly.state == TURN_STREAMING &&
           agent_signal_stops(signal, &stream->assembly);
}

int agent_stream_has_usage(const struct agent_stream *stream)
{
    return attempt_log_has_usage(&stream->attempts);
}

int agent_stream_rejected_for_size(const struct agent_stream *stream, long context_limit)
{
    if (stream->assembly.state != TURN_FAILED)
        return 0;
    /* A known window keeps room for the reply before compacting, so a length limit there is an
     * output cap, such as a provider's default max_tokens, not a full context. */
    if (stream->error_kind == API_ERROR_LENGTH_LIMIT)
        return context_limit <= 0;
    return stream->error_kind == API_ERROR_CONTEXT_OVERFLOW ||
           stream->error_kind == API_ERROR_PAYLOAD_TOO_LARGE;
}
