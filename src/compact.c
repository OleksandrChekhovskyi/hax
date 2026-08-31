/* SPDX-License-Identifier: MIT */
#include "compact.h"

#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "agent_usage.h"
#include "config.h"
#include "model_meta.h"
#include "provider.h"
#include "session.h"
#include "transcript.h"
#include "turn.h"
#include "xalloc.h"
#include "system/clock.h"

/* Fixed sections and exact identifiers make the seed useful across model changes. The leading
 * output-only instruction prevents weaker models from continuing the task instead. */
static const char COMPACT_PROMPT[] =
    "Summarize the conversation so far into a structured context checkpoint that lets the "
    "work continue without access to the full history above.\n"
    "\n"
    "CRITICAL: Respond with the summary text ONLY. Do not call any tools and do not continue "
    "the task — your entire reply is the summary.\n"
    "\n"
    "Use this exact Markdown structure, keeping every section even when empty:\n"
    "\n"
    "## Goal\n"
    "- [what the user is ultimately trying to accomplish]\n"
    "\n"
    "## Constraints & Preferences\n"
    "- [explicit user constraints, preferences, or requirements, or \"(none)\"]\n"
    "\n"
    "## Progress\n"
    "### Done\n"
    "- [completed work, or \"(none)\"]\n"
    "### In Progress\n"
    "- [work underway right now, or \"(none)\"]\n"
    "### Blocked\n"
    "- [blockers, or \"(none)\"]\n"
    "\n"
    "## Key Decisions\n"
    "- [decision: brief rationale, or \"(none)\"]\n"
    "\n"
    "## Files\n"
    "- [path: why it matters / what changed, or \"(none)\"]\n"
    "\n"
    "## Next Steps\n"
    "- [ordered next actions, or \"(none)\"]\n"
    "\n"
    "## Critical Context\n"
    "- [important technical facts, error strings, identifiers, commands, open questions, or "
    "\"(none)\"]\n"
    "\n"
    "Rules:\n"
    "- Be terse: bullets, not prose paragraphs.\n"
    "- Preserve exact file paths, function names, commands, and error strings.\n"
    "- Do not mention that a summary was produced or that context was compacted.";

static const char COMPACT_SEED_PREAMBLE[] =
    "The earlier part of this conversation was condensed to free up context. The summary "
    "below captures everything that happened before this point — treat it as established "
    "context and continue the work from here.";

int compact_auto_enabled(void)
{
    return config_bool("compact.auto");
}

int compact_over_threshold(long context_tokens, long context_limit, int threshold_percent)
{
    if (context_tokens < 0 || context_limit <= 0 || threshold_percent < 1 ||
        threshold_percent > 100)
        return 0;

    long threshold_tokens = context_limit / 100 * threshold_percent;
    long scaled_remainder = context_limit % 100 * threshold_percent;
    threshold_tokens += scaled_remainder / 100 + (scaled_remainder % 100 != 0);
    return context_tokens >= threshold_tokens;
}

int compact_should_auto(long context_tokens, long context_limit)
{
    if (!compact_auto_enabled())
        return 0;
    return compact_over_threshold(context_tokens, context_limit, config_int("compact.threshold"));
}

static char *build_summary_prompt(const char *instructions)
{
    if (instructions && *instructions)
        return xasprintf("%s\n\nAdditional focus for this summary:\n%s", COMPACT_PROMPT,
                         instructions);
    return xstrdup(COMPACT_PROMPT);
}

static char *build_summary_seed(const char *summary)
{
    return xasprintf("%s\n\n%s", COMPACT_SEED_PREAMBLE, summary);
}

#define COMPACT_MAX_ATTEMPTS 4

struct compact_attempt {
    struct stream_usage usage;
    long elapsed_ms;
    /* Owned; the terminal event's stream_response only borrows its strings. */
    char *response_id;
    char *served_model;
    char *route;
};

struct compact_attempt_log {
    struct compact_attempt attempts[COMPACT_MAX_ATTEMPTS];
    size_t count;
    long attempt_started_ms;
    /* Usage banked by mid-stream retries, folded into the next terminal entry. */
    struct stream_usage pending_retry;
};

struct compact_sink {
    struct turn turn;
    struct compact_attempt_log attempt_log;
    const struct compact_hooks *hooks;
    char *error_message;
};

struct summary_request {
    struct context base_context;
    struct item *items;
    size_t count;
    size_t capacity;
    size_t borrowed_count;
};

static void attempt_log_init(struct compact_attempt_log *log)
{
    log->count = 0;
    log->attempt_started_ms = monotonic_ms();
    log->pending_retry = (struct stream_usage){-1, -1, -1, -1, -1, -1};
}

static void attempt_log_record(struct compact_attempt_log *log, const struct stream_usage *usage,
                               const struct stream_response *response)
{
    long now_ms = monotonic_ms();
    if (log->count < COMPACT_MAX_ATTEMPTS) {
        struct compact_attempt *attempt = &log->attempts[log->count++];
        attempt->usage = *usage;
        agent_usage_add(&attempt->usage, &log->pending_retry);
        attempt->elapsed_ms = now_ms - log->attempt_started_ms;
        attempt->response_id = response && response->id ? xstrdup(response->id) : NULL;
        attempt->served_model = response && response->model ? xstrdup(response->model) : NULL;
        attempt->route = response && response->route ? xstrdup(response->route) : NULL;
    }
    log->pending_retry = (struct stream_usage){-1, -1, -1, -1, -1, -1};
    log->attempt_started_ms = now_ms;
}

/* A stream cancelled mid-retry banks usage but never reaches a terminal event; record it as
 * its own entry so persisted usage matches what the live accounting hooks already billed. */
static void attempt_log_flush(struct compact_attempt_log *log)
{
    if (!agent_usage_is_reported(&log->pending_retry))
        return;
    struct stream_usage usage = log->pending_retry;
    log->pending_retry = (struct stream_usage){-1, -1, -1, -1, -1, -1};
    attempt_log_record(log, &usage, NULL);
}

static void attempt_log_free(struct compact_attempt_log *log)
{
    for (size_t i = 0; i < log->count; i++) {
        free(log->attempts[i].response_id);
        free(log->attempts[i].served_model);
        free(log->attempts[i].route);
    }
    log->count = 0;
}

static int compact_sink_on_event(const struct stream_event *event, void *user)
{
    struct compact_sink *sink = user;
    const struct stream_usage *usage = NULL;
    const struct stream_response *response = NULL;

    if (event->kind == EV_DONE) {
        usage = &event->u.done.usage;
        response = &event->u.done.response;
    } else if (event->kind == EV_ERROR) {
        if (!sink->error_message)
            sink->error_message =
                xstrdup(event->u.error.message ? event->u.error.message : "stream failed");
        usage = event->u.error.usage;
        response = event->u.error.response;
    } else if (event->kind == EV_RETRY && event->u.retry.usage) {
        agent_usage_add(&sink->attempt_log.pending_retry, event->u.retry.usage);
    }
    if (usage)
        attempt_log_record(&sink->attempt_log, usage, response);
    if (sink->hooks->on_event)
        sink->hooks->on_event(event, sink->hooks->user);
    turn_consume(&sink->turn, event);
    return 0;
}

/* Ownership of `item` transfers to `request`. */
static void summary_request_append(struct summary_request *request, struct item item)
{
    if (request->count == request->capacity) {
        request->capacity = request->capacity ? request->capacity * 2 : 16;
        request->items = xrealloc(request->items, request->capacity * sizeof(*request->items));
    }
    request->items[request->count++] = item;
}

static void summary_request_init(struct summary_request *request,
                                 const struct compact_params *params)
{
    memset(request, 0, sizeof(*request));
    request->base_context = agent_session_context(params->session);
    request->base_context.image_input =
        model_meta_image_input(params->provider, params->session->model);
    request->borrowed_count = request->base_context.n_items;
    request->capacity = request->borrowed_count + 1;
    request->items = xmalloc(request->capacity * sizeof(*request->items));
    if (request->borrowed_count > 0)
        memcpy(request->items, request->base_context.items,
               request->borrowed_count * sizeof(*request->items));
    request->count = request->borrowed_count;
    summary_request_append(request, (struct item){
                                        .kind = ITEM_USER_MESSAGE,
                                        .text = build_summary_prompt(params->instructions),
                                    });
}

static void summary_request_destroy(struct summary_request *request)
{
    for (size_t i = request->borrowed_count; i < request->count; i++)
        item_free(&request->items[i]);
    free(request->items);
}

static int response_has_tool_call(const struct item *items, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (items[i].kind == ITEM_TOOL_CALL)
            return 1;
    return 0;
}

static char *extract_summary(const struct item *items, size_t count)
{
    const char *summary = NULL;

    for (size_t i = 0; i < count; i++)
        if (items[i].kind == ITEM_ASSISTANT_MESSAGE && items[i].text && *items[i].text)
            summary = items[i].text;
    return summary ? xstrdup(summary) : NULL;
}

static void free_items(struct item *items, size_t count)
{
    for (size_t i = 0; i < count; i++)
        item_free(&items[i]);
    free(items);
}

static const char TOOL_CALL_REJECTION[] =
    "[rejected] Tool calls are disabled while summarizing. Respond with the summary text "
    "only — do not call any tools.";

/* The request takes ownership of every response item. */
static void append_rejected_response(struct summary_request *request, struct item *items,
                                     size_t count)
{
    size_t response_start = request->count;
    for (size_t i = 0; i < count; i++)
        summary_request_append(request, items[i]);
    free(items);

    size_t response_end = request->count;
    for (size_t i = response_start; i < response_end; i++) {
        if (request->items[i].kind != ITEM_TOOL_CALL)
            continue;
        char *call_id = request->items[i].call_id ? xstrdup(request->items[i].call_id) : NULL;
        summary_request_append(request, (struct item){
                                            .kind = ITEM_TOOL_RESULT,
                                            .call_id = call_id,
                                            .output = xstrdup(TOOL_CALL_REJECTION),
                                        });
    }
}

static char *generate_summary(const struct compact_params *params, struct compact_sink *sink,
                              int *attempt_count)
{
    struct summary_request request;
    summary_request_init(&request, params);
    char *summary = NULL;

    /* Advertising the normal tools preserves the provider's cached request prefix. Calls are
     * rejected rather than executed because compaction must not mutate task state. */
    for (int attempt = 0; attempt < COMPACT_MAX_ATTEMPTS; attempt++) {
        struct context context = request.base_context;
        context.items = request.items;
        context.n_items = request.count;
        (*attempt_count)++;
        params->provider->stream(params->provider, &context, params->session->model,
                                 compact_sink_on_event, sink, params->hooks.tick,
                                 params->hooks.user);

        if (sink->turn.state != TURN_DONE)
            break;

        size_t response_count;
        struct item *response = turn_take_items(&sink->turn, &response_count);
        if (!response_has_tool_call(response, response_count)) {
            summary = extract_summary(response, response_count);
            free_items(response, response_count);
            break;
        }
        if (attempt == COMPACT_MAX_ATTEMPTS - 1) {
            free_items(response, response_count);
            break;
        }

        append_rejected_response(&request, response, response_count);
        turn_reset(&sink->turn);
    }

    summary_request_destroy(&request);
    return summary;
}

/* The boundary prevents the seed from rendering as part of the preceding assistant turn. */
static void append_summary_seed(struct agent_session *session, const char *summary)
{
    agent_session_add_boundary(session);
    agent_session_append(session, (struct item){
                                      .kind = ITEM_USER_MESSAGE,
                                      .text = build_summary_seed(summary),
                                      .origin = ITEM_ORIGIN_COMPACT_SEED,
                                  });
}

static void flush_compaction_logs(const struct compact_params *params)
{
    transcript_log_append(params->transcript_log, params->session->items, params->session->n_items);
    session_log_append(params->session_log, params->session->items, params->session->n_items);
}

static void append_attempt_usage(const struct compact_params *params,
                                 const struct compact_attempt_log *log, size_t first, size_t end)
{
    for (size_t i = first; i < end; i++) {
        struct stream_response response = {
            .id = log->attempts[i].response_id,
            .model = log->attempts[i].served_model,
            .route = log->attempts[i].route,
        };
        agent_session_add_turn_usage(params->session, params->provider, &log->attempts[i].usage,
                                     log->attempts[i].elapsed_ms, &response);
    }
}

void compact_run(const struct compact_params *params, struct compact_result *result)
{
    memset(result, 0, sizeof(*result));
    if (!params->provider) {
        result->outcome = COMPACT_NO_PROVIDER;
        return;
    }
    if (!params->session->model || !*params->session->model) {
        result->outcome = COMPACT_NO_MODEL;
        return;
    }
    if (params->session->n_items == 0) {
        result->outcome = COMPACT_EMPTY;
        return;
    }

    /* A resumed session may compact before its first ordinary turn, so resolve effort here too. */
    if (agent_session_resync_effort(params->session, params->provider, NULL))
        session_log_set_meta(params->session_log, agent_provider_log_name(params->provider),
                             params->session->model, params->session->model_label,
                             params->session->effort, config_str("preset"));

    struct compact_sink sink = {.hooks = &params->hooks};
    turn_init(&sink.turn);
    attempt_log_init(&sink.attempt_log);
    char *summary = generate_summary(params, &sink, &result->attempts);
    int cancelled = params->hooks.is_cancelled && params->hooks.is_cancelled(params->hooks.user);
    if (cancelled) {
        free(summary);
        summary = NULL;
    }
    turn_reset(&sink.turn);
    result->error_message = sink.error_message;
    attempt_log_flush(&sink.attempt_log);

    if (!summary || !*summary) {
        append_attempt_usage(params, &sink.attempt_log, 0, sink.attempt_log.count);
        attempt_log_free(&sink.attempt_log);
        flush_compaction_logs(params);
        free(summary);
        if (cancelled)
            result->outcome = COMPACT_CANCELLED;
        else if (result->error_message)
            result->outcome = COMPACT_PROVIDER_ERROR;
        else
            result->outcome = COMPACT_NO_SUMMARY;
        return;
    }

    /* Rejected attempts are part of the summarized prefix; accepted usage follows the seed. */
    size_t accepted_usage_index = sink.attempt_log.count ? sink.attempt_log.count - 1 : 0;
    append_attempt_usage(params, &sink.attempt_log, 0, accepted_usage_index);
    append_summary_seed(params->session, summary);
    free(summary);
    append_attempt_usage(params, &sink.attempt_log, accepted_usage_index, sink.attempt_log.count);
    attempt_log_free(&sink.attempt_log);
    flush_compaction_logs(params);
    result->outcome = COMPACT_COMPLETE;
}

void compact_result_destroy(struct compact_result *result)
{
    free(result->error_message);
    result->error_message = NULL;
}
