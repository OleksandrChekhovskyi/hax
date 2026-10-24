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

static const char SHRUNK_NOTE[] =
    "Parts of the conversation above were shortened or omitted to fit the model's context window. "
    "Summarize what remains, and note under Critical Context that earlier details may be missing.";

static char *build_summary_prompt(const char *instructions, int shrunk)
{
    return xasprintf("%s%s%s%s%s", COMPACT_PROMPT, shrunk ? "\n\n" : "", shrunk ? SHRUNK_NOTE : "",
                     instructions && *instructions ? "\n\nAdditional focus for this summary:\n"
                                                   : "",
                     instructions && *instructions ? instructions : "");
}

/* A longer request is left to the summary: quoting it would make the seed nearly as large as a
 * request the model may just have refused. */
#define KEPT_REQUEST_MAX_BYTES 8192

/* The newest prompt the user typed, or NULL. It may predate the context window when an earlier
 * compaction in the same user turn already summarized it. */
static const char *latest_request(const struct agent_session *session)
{
    for (size_t i = session->n_items; i-- > 0;) {
        const struct item *item = &session->items[i];
        if (item->kind == ITEM_USER_MESSAGE && item->origin == ITEM_ORIGIN_NONE)
            return item->text;
    }
    return NULL;
}

/* `request` is quoted after the summary when non-NULL. */
static char *build_summary_seed(const char *summary, const char *request)
{
    if (request)
        return xasprintf("%s\n\n%s\n\nThe request being worked on, verbatim:\n\n%s",
                         COMPACT_SEED_PREAMBLE, summary, request);
    return xasprintf("%s\n\n%s", COMPACT_SEED_PREAMBLE, summary);
}

#define COMPACT_MAX_ATTEMPTS 4

struct compact_sink {
    struct turn turn;
    struct attempt_log attempt_log;
    const struct compact_hooks *hooks;
    /* The latest failure, so a later request's error replaces an earlier refusal. */
    char *error_message;
    /* The latest attempt was refused for size, so a smaller request may still fit. */
    int overflow;
};

struct summary_request {
    struct context base_context;
    struct item *items;
    size_t count;
    size_t capacity;
    size_t borrowed_count;
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
        free(sink->error_message);
        sink->error_message =
            xstrdup(event->u.error.message ? event->u.error.message : "stream failed");
        sink->overflow = event->u.error.error_kind == STREAM_ERROR_CONTEXT_OVERFLOW;
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

/* `items` are borrowed for the request's lifetime; `shrunk` marks them as a shortened view. */
static void summary_request_init(struct summary_request *request,
                                 const struct compact_params *params, const struct item *items,
                                 size_t n_items, int shrunk)
{
    memset(request, 0, sizeof(*request));
    request->base_context = agent_session_context(params->session);
    request->base_context.image_input =
        model_meta_image_input(params->provider, params->session->model);
    request->base_context.session_id = session_log_id(params->session_log);
    request->borrowed_count = n_items;
    request->capacity = request->borrowed_count + 1;
    request->items = xmalloc(request->capacity * sizeof(*request->items));
    if (request->borrowed_count > 0)
        memcpy(request->items, items, request->borrowed_count * sizeof(*request->items));
    request->count = request->borrowed_count;
    summary_request_append(request, (struct item){
                                        .kind = ITEM_USER_MESSAGE,
                                        .text = build_summary_prompt(params->instructions, shrunk),
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
                              const struct item *items, size_t n_items, int shrunk,
                              int *attempt_count)
{
    struct summary_request request;
    summary_request_init(&request, params, items, n_items, shrunk);
    char *summary = NULL;
    /* A refused request left a failed assembly behind. */
    turn_reset(&sink->turn);

    /* Advertising the normal tools preserves the provider's cached request prefix. Calls are
     * rejected rather than executed because compaction must not mutate task state. */
    for (int attempt = 0; attempt < COMPACT_MAX_ATTEMPTS; attempt++) {
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

/* A summarization request the model refuses for size is retried on shortened views of the
 * conversation, each smaller than the last. The session itself is never trimmed. */
enum shrink_stage {
    SHRINK_CLIP,        /* clip bulky tool output and arguments */
    SHRINK_OLDER_HALF,  /* also omit the older half of the earlier rounds */
    SHRINK_LATEST_ONLY, /* also omit every round but the latest request's and the last */
    SHRINK_STAGES,
};

static const size_t SHRINK_CLIP_BYTES[SHRINK_STAGES] = {4096, 4096, 1024};

static const char OMITTED_ROUNDS[] =
    "[Earlier exchanges were omitted here to fit the model's context window.]";

/* Shallow copies of session items: they borrow every pointer except the replacement strings in
 * `strings`, which the view owns. */
struct summary_view {
    struct item *items;
    size_t n_items;
    size_t capacity;
    char **strings;
    size_t n_strings;
    size_t bytes; /* approximate content size, for comparing requests */
};

static size_t text_bytes(const char *text)
{
    return text ? strlen(text) : 0;
}

static size_t item_bytes(const struct item *item)
{
    size_t bytes = text_bytes(item->text) + text_bytes(item->tool_arguments_json) +
                   text_bytes(item->output) + text_bytes(item->reasoning_json) +
                   text_bytes(item->reasoning_text);
    for (size_t i = 0; i < item->n_images; i++)
        bytes += text_bytes(item->images[i].data_b64);
    return bytes;
}

static size_t items_bytes(const struct item *items, size_t n_items)
{
    size_t bytes = 0;
    for (size_t i = 0; i < n_items; i++)
        bytes += item_bytes(&items[i]);
    return bytes;
}

static char *view_own(struct summary_view *view, char *text)
{
    view->strings = xrealloc(view->strings, (view->n_strings + 1) * sizeof(*view->strings));
    view->strings[view->n_strings++] = text;
    return text;
}

static void view_append(struct summary_view *view, struct item item)
{
    if (view->n_items == view->capacity) {
        view->capacity = view->capacity ? view->capacity * 2 : 16;
        view->items = xrealloc(view->items, view->capacity * sizeof(*view->items));
    }
    view->items[view->n_items++] = item;
    view->bytes += item_bytes(&item);
}

static void view_free(struct summary_view *view)
{
    for (size_t i = 0; i < view->n_strings; i++)
        free(view->strings[i]);
    free(view->strings);
    free(view->items);
}

/* The leading `cap` bytes of `output`, cut at a character boundary, with image parts replaced by
 * their placeholders and the omission stated. */
static char *clip_output(const struct item *result, size_t cap)
{
    const char *output = result->output ? result->output : "";
    size_t len = strlen(output);
    size_t keep = len;
    if (len > cap) {
        keep = cap;
        while (keep > 0 && ((unsigned char)output[keep] & 0xC0) == 0x80)
            keep--;
    }

    char *clipped = xasprintf("%.*s", (int)keep, output);
    for (size_t i = 0; i < result->n_images; i++) {
        char *placeholder = item_image_placeholder(&result->images[i]);
        char *joined = xasprintf("%s\n%s", clipped, placeholder);
        free(placeholder);
        free(clipped);
        clipped = joined;
    }
    if (keep < len) {
        char *joined = xasprintf("%s\n[%zu bytes omitted to fit the model's context window]",
                                 clipped, len - keep);
        free(clipped);
        clipped = joined;
    }
    return clipped;
}

static void view_append_clipped(struct summary_view *view, const struct item *item, size_t cap)
{
    struct item copy = *item;
    if (item->kind == ITEM_TOOL_RESULT && (text_bytes(item->output) > cap || item->n_images > 0)) {
        copy.output = view_own(view, clip_output(item, cap));
        copy.output_hidden_tail = 0;
        copy.images = NULL;
        copy.n_images = 0;
    } else if (item->kind == ITEM_TOOL_CALL && text_bytes(item->tool_arguments_json) > cap) {
        /* Adapters send arguments as JSON, so the stand-in must stay a JSON object. */
        copy.tool_arguments_json = view_own(
            view, xasprintf("{\"omitted\":\"%zu bytes of arguments omitted to fit the model's "
                            "context window\"}",
                            strlen(item->tool_arguments_json)));
    }
    view_append(view, copy);
}

/* Rounds start at index 0 and at each turn boundary, so a round holds whole tool exchanges and
 * omitting one never separates a call from its result. Returns the round count; `starts` is
 * owned by the caller. */
static size_t split_rounds(const struct item *items, size_t n_items, size_t **starts)
{
    size_t n_rounds = 0;
    *starts = xmalloc((n_items + 1) * sizeof(**starts));
    for (size_t i = 0; i < n_items; i++)
        if (i == 0 || items[i].kind == ITEM_TURN_BOUNDARY)
            (*starts)[n_rounds++] = i;
    (*starts)[n_rounds] = n_items;
    return n_rounds;
}

/* The latest request is the newest prompt the user typed; the summary must cover it whole. */
static size_t latest_request_round(const struct item *items, const size_t *starts, size_t n_rounds)
{
    for (size_t round = n_rounds; round-- > 0;)
        for (size_t i = starts[round]; i < starts[round + 1]; i++)
            if (items[i].kind == ITEM_USER_MESSAGE && items[i].origin == ITEM_ORIGIN_NONE)
                return round;
    return n_rounds - 1;
}

static void build_view(const struct item *items, size_t n_items, enum shrink_stage stage,
                       struct summary_view *view)
{
    memset(view, 0, sizeof(*view));
    if (n_items == 0)
        return;

    size_t *starts;
    size_t n_rounds = split_rounds(items, n_items, &starts);
    size_t request_round = latest_request_round(items, starts, n_rounds);
    /* An earlier seed already condenses everything before it, so it is the last to go. */
    int seed_round = items[0].origin == ITEM_ORIGIN_COMPACT_SEED;
    char *keep = xmalloc(n_rounds);
    memset(keep, 1, n_rounds);

    if (stage >= SHRINK_OLDER_HALF) {
        size_t droppable_bytes = 0;
        for (size_t round = seed_round; round < request_round; round++)
            droppable_bytes +=
                items_bytes(items + starts[round], starts[round + 1] - starts[round]);
        size_t dropped_bytes = 0;
        for (size_t round = seed_round; round + 1 < n_rounds; round++) {
            if (round == request_round)
                continue;
            int older = round < request_round;
            if (stage == SHRINK_OLDER_HALF && (!older || dropped_bytes * 2 >= droppable_bytes))
                continue;
            keep[round] = 0;
            dropped_bytes += items_bytes(items + starts[round], starts[round + 1] - starts[round]);
        }
        if (stage == SHRINK_LATEST_ONLY && seed_round && request_round > 0 && n_rounds > 1)
            keep[0] = 0;
    }

    for (size_t round = 0; round < n_rounds; round++) {
        if (!keep[round]) {
            if (round == 0 || keep[round - 1])
                view_append(
                    view, (struct item){.kind = ITEM_USER_MESSAGE, .text = (char *)OMITTED_ROUNDS});
            continue;
        }
        for (size_t i = starts[round]; i < starts[round + 1]; i++)
            view_append_clipped(view, &items[i], SHRINK_CLIP_BYTES[stage]);
    }
    free(keep);
    free(starts);
}

/* Retry a refused summarization on shortened views. Each request must be materially smaller
 * than the refused one; a stage that is not is skipped rather than sent. */
static char *generate_shrunk_summary(const struct compact_params *params, struct compact_sink *sink,
                                     const struct item *items, size_t n_items, int *attempt_count,
                                     int *shrunk)
{
    size_t refused_bytes = items_bytes(items, n_items);

    for (int stage = 0; stage < SHRINK_STAGES; stage++) {
        if (params->hooks.is_cancelled && params->hooks.is_cancelled(params->hooks.user))
            return NULL;

        struct summary_view view;
        build_view(items, n_items, (enum shrink_stage)stage, &view);
        if (view.bytes * 10 > refused_bytes * 9) {
            view_free(&view);
            continue;
        }
        char *summary = generate_summary(params, sink, view.items, view.n_items, 1, attempt_count);
        refused_bytes = view.bytes;
        view_free(&view);
        if (summary) {
            *shrunk = 1;
            return summary;
        }
        if (!sink->overflow)
            return NULL;
    }

    char *refusal = sink->error_message;
    sink->error_message =
        xasprintf("%s — the conversation cannot be shortened enough to fit the model's context "
                  "window",
                  refusal ? refusal : "context window exceeded");
    free(refusal);
    return NULL;
}

/* The boundary prevents the seed from rendering as part of the preceding assistant turn. */
static void append_summary_seed(struct agent_session *session, const char *summary,
                                const char *request)
{
    char *seed = build_summary_seed(summary, request);
    agent_session_add_boundary(session);
    agent_session_append(session, (struct item){
                                      .kind = ITEM_USER_MESSAGE,
                                      .text = seed,
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
        generate_summary(params, &sink, window.items, window.n_items, 0, &result->attempts);
    if (!summary && sink.overflow)
        summary = generate_shrunk_summary(params, &sink, window.items, window.n_items,
                                          &result->attempts, &result->shrunk);
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
    const char *request = params->keep_request ? latest_request(params->session) : NULL;
    if (request && strlen(request) > KEPT_REQUEST_MAX_BYTES)
        request = NULL;
    append_summary_seed(params->session, summary, request);
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
