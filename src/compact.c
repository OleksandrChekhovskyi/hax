/* SPDX-License-Identifier: MIT */
#include "compact.h"

#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "agent_stream.h"
#include "agent_usage.h"
#include "config.h"
#include "model_meta.h"
#include "provider.h"
#include "session.h"
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

static int compact_auto_enabled(void)
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

static int compact_should_auto(long context_tokens, long context_limit)
{
    if (!compact_auto_enabled())
        return 0;
    return compact_over_threshold(context_tokens, context_limit, config_int("compact.threshold"));
}

int compact_due(const struct agent_session *session, struct provider *provider)
{
    if (!provider || !session->model)
        return 0;
    return compact_should_auto(agent_session_last_context_tokens(session),
                               model_meta_context(provider, session->model));
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

struct summary_request {
    struct context base_context;
    struct item *items;
    size_t count;
    size_t capacity;
    size_t borrowed_count;
};

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
    request->base_context.session_id = session_log_id(params->session_log);
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

static char *generate_summary(const struct compact_params *params, struct agent_stream *stream,
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
        agent_stream_request(stream, params->provider, &context, params->session->model, NULL, NULL,
                             params->hooks.poll, params->hooks.user);

        if (stream->assembly.state != TURN_DONE)
            break;

        size_t response_count;
        struct item *response = turn_take_items(&stream->assembly, &response_count);
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
    agent_flush_logs(params->transcript_log, params->session_log, params->session->items,
                     params->session->n_items);
}

static void append_attempt_usage(const struct compact_params *params, const struct attempt_log *log,
                                 size_t first, size_t end)
{
    for (size_t i = first; i < end; i++)
        agent_session_add_attempt_usage(params->session, params->provider, &log->attempts[i],
                                        ITEM_ORIGIN_COMPACTION);
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

    struct agent_stream stream;
    agent_stream_init(&stream);
    char *summary = generate_summary(params, &stream, &result->attempts);
    /* An abort rejects even a complete summary; a pause rejects only the request it cut. */
    enum agent_signal signal =
        params->hooks.checkpoint ? params->hooks.checkpoint(params->hooks.user) : AGENT_SIGNAL_NONE;
    int cancelled = signal == AGENT_SIGNAL_ABORT || agent_stream_cut_by(&stream, signal);
    if (cancelled) {
        free(summary);
        summary = NULL;
    }
    result->error_message = stream.error_message;
    stream.error_message = NULL;
    const struct attempt_log *attempts = &stream.attempts;

    if (!summary || !*summary) {
        if (cancelled)
            result->outcome = COMPACT_CANCELLED;
        else if (stream.assembly.state == TURN_FAILED)
            result->outcome = COMPACT_PROVIDER_ERROR;
        else
            result->outcome = COMPACT_NO_SUMMARY;
        append_attempt_usage(params, attempts, 0, attempts->count);
        agent_stream_destroy(&stream);
        flush_compaction_logs(params);
        free(summary);
        return;
    }

    /* Rejected attempts are part of the summarized prefix; accepted usage follows the seed. */
    size_t accepted_usage_index = attempts->count ? attempts->count - 1 : 0;
    append_attempt_usage(params, attempts, 0, accepted_usage_index);
    append_summary_seed(params->session, summary);
    free(summary);
    append_attempt_usage(params, attempts, accepted_usage_index, attempts->count);
    agent_stream_destroy(&stream);
    flush_compaction_logs(params);
    result->outcome = COMPACT_COMPLETE;
}

void compact_result_destroy(struct compact_result *result)
{
    free(result->error_message);
    result->error_message = NULL;
}
