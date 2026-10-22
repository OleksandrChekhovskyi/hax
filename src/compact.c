/* SPDX-License-Identifier: MIT */
#include "compact.h"

#include <ctype.h>
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

/* The wordings providers use for a request the model cannot take. A request over the limit is
 * rejected outright, so the phrase is what tells hax to summarize instead of failing. */
static const char *const OVERFLOW_PHRASES[] = {
    "context length exceeded",
    "context_length_exceeded",
    "maximum context length",
    "context window exceeded",
    "exceeds the context window",
    "context size exceeded",
    "exceeds the available context size",
    "prompt is too long",
    "input is too long",
    "too many tokens",
    "exceeds the maximum number of tokens",
    "reduce the length of the messages",
    NULL,
};

static int contains_fold(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);

    for (const char *cursor = haystack; *cursor; cursor++) {
        size_t i = 0;
        while (i < needle_len && cursor[i] &&
               tolower((unsigned char)cursor[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == needle_len)
            return 1;
    }
    return 0;
}

int compact_error_is_overflow(const char *message)
{
    if (!message || !*message)
        return 0;

    for (size_t i = 0; OVERFLOW_PHRASES[i]; i++)
        if (contains_fold(message, OVERFLOW_PHRASES[i]))
            return 1;
    return 0;
}

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

struct compact_sink {
    struct turn turn;
    struct attempt_log attempt_log;
    const struct compact_hooks *hooks;
    char *error_message;
    /* The last attempt was rejected for size, so a smaller request may still fit. */
    int overflow;
};

struct summary_request {
    struct context base_context;
    struct item *items;
    size_t count;
    size_t capacity;
    /* Items before this index copy borrowed items; the ones from it on own their fields. */
    size_t owned_from;
};

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
        if (compact_error_is_overflow(event->u.error.message))
            sink->overflow = 1;
        usage = event->u.error.usage;
        response = event->u.error.response;
    } else if (event->kind == EV_RETRY) {
        attempt_log_retry(&sink->attempt_log, event->u.retry.usage, event->u.retry.delay_ms);
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

/* `carried` is the item standing in for an already-summarized older prefix, or NULL. The caller
 * owns it and keeps it alive for the request; `items` are borrowed from the session. */
static void summary_request_init(struct summary_request *request,
                                 const struct compact_params *params, const struct item *carried,
                                 const struct item *items, size_t n_items)
{
    memset(request, 0, sizeof(*request));
    request->base_context = agent_session_context(params->session);
    request->base_context.image_input =
        model_meta_image_input(params->provider, params->session->model);
    request->base_context.session_id = session_log_id(params->session_log);
    request->capacity = (carried ? 1 : 0) + n_items + 1;
    request->items = xmalloc(request->capacity * sizeof(*request->items));

    size_t count = 0;
    if (carried)
        request->items[count++] = *carried;
    if (n_items) {
        memcpy(request->items + count, items, n_items * sizeof(*request->items));
        count += n_items;
    }
    request->count = count;
    request->owned_from = count;
    summary_request_append(request, (struct item){
                                        .kind = ITEM_USER_MESSAGE,
                                        .text = build_summary_prompt(params->instructions),
                                    });
}

static void summary_request_destroy(struct summary_request *request)
{
    for (size_t i = request->owned_from; i < request->count; i++)
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

/* The provider requests one compaction may spend. A history too large for the model is
 * summarized in pieces, and the pieces must not become an unbounded series of requests. */
#define COMPACT_MAX_REQUESTS 16

static char *generate_summary(const struct compact_params *params, struct compact_sink *sink,
                              const struct item *items, size_t n_items, const struct item *carried,
                              int *attempt_count)
{
    struct summary_request request;
    summary_request_init(&request, params, carried, items, n_items);
    char *summary = NULL;
    /* Each chunk starts from an empty assembly: the previous one left a terminal state behind. */
    turn_reset(&sink->turn);

    /* Advertising the normal tools preserves the provider's cached request prefix. Calls are
     * rejected rather than executed because compaction must not mutate task state. */
    for (int attempt = 0; attempt < COMPACT_MAX_ATTEMPTS; attempt++) {
        if (*attempt_count >= COMPACT_MAX_REQUESTS)
            break;
        struct context context = request.base_context;
        context.items = request.items;
        context.n_items = request.count;
        (*attempt_count)++;
        sink->overflow = 0;
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

/* The split lands where a turn starts, so both halves hold whole turns and neither carries a
 * tool call away from its result. */
static int is_turn_start(const struct item *item)
{
    return item->kind == ITEM_TURN_BOUNDARY || item->kind == ITEM_USER_MESSAGE;
}

/* The index where the range splits, closest to its middle. Zero when the range has no turn
 * boundary to split at. */
static size_t split_index(const struct item *items, size_t n_items)
{
    size_t middle = n_items / 2;
    size_t best = 0;
    size_t best_distance = 0;

    for (size_t i = 1; i < n_items; i++) {
        if (!is_turn_start(&items[i]))
            continue;
        size_t distance = i > middle ? i - middle : middle - i;
        if (!best || distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    return best;
}

/* Summarize the range, with `carried` standing in for an already-summarized older prefix. When
 * the model rejects the request for size, the range splits: the older part is summarized first
 * and its summary then stands in for that part, so every request stays within the model's
 * capacity and the result still covers the whole range. */
static char *summarize_range(const struct compact_params *params, struct compact_sink *sink,
                             const struct item *items, size_t n_items, const char *carried,
                             int *attempt_count)
{
    if (*attempt_count >= COMPACT_MAX_REQUESTS)
        return NULL;

    struct item carried_item = {0};
    if (carried) {
        carried_item.kind = ITEM_USER_MESSAGE;
        carried_item.text = build_summary_seed(carried);
        carried_item.origin = ITEM_ORIGIN_COMPACT_SEED;
    }

    char *summary = generate_summary(params, sink, items, n_items, carried ? &carried_item : NULL,
                                     attempt_count);
    item_free(&carried_item);
    if (summary)
        return summary;
    if (!sink->overflow)
        return NULL;

    size_t split = split_index(items, n_items);
    if (split == 0 || split >= n_items)
        return NULL;

    char *older = summarize_range(params, sink, items, split, carried, attempt_count);
    if (!older)
        return NULL;

    char *result =
        summarize_range(params, sink, items + split, n_items - split, older, attempt_count);
    free(older);
    return result;
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

static void append_attempt_usage(const struct compact_params *params, const struct attempt_log *log,
                                 size_t first, size_t end)
{
    for (size_t i = first; i < end; i++) {
        struct stream_response response = {
            .id = log->attempts[i].response_id,
            .model = log->attempts[i].served_model,
            .route = log->attempts[i].route,
        };
        agent_session_add_turn_usage(params->session, params->provider, &log->attempts[i].usage,
                                     log->attempts[i].elapsed_ms, &response,
                                     ITEM_ORIGIN_COMPACTION);
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
    struct context window = agent_session_context(params->session);
    char *summary =
        summarize_range(params, &sink, window.items, window.n_items, NULL, &result->attempts);
    int cancelled = params->hooks.is_cancelled && params->hooks.is_cancelled(params->hooks.user);
    if (cancelled) {
        free(summary);
        summary = NULL;
    }
    turn_reset(&sink.turn);
    result->error_message = sink.error_message;

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
