/* SPDX-License-Identifier: MIT */
#include "compact.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "catalog.h"
#include "config.h"
#include "provider.h"
#include "session.h"
#include "tool.h"
#include "transcript.h"
#include "turn.h"
#include "util.h"

/* Structured-checkpoint template. Mirrors the shape proven out by the
 * reference agents (opencode/pi): fixed sections, terse bullets, preserve
 * exact identifiers. The leading no-tools / output-only framing matters for
 * weaker local models that will otherwise try to keep working the task. */
static const char *const COMPACT_PROMPT =
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

static const char *const COMPACT_SEED_PREAMBLE =
    "The earlier part of this conversation was condensed to free up context. The summary "
    "below captures everything that happened before this point — treat it as established "
    "context and continue the work from here.";

int compact_auto_enabled(void)
{
    return config_bool("compact.auto");
}

int compact_over_threshold(long ctx_tokens, long limit, int pct)
{
    if (limit <= 0 || ctx_tokens < 0)
        return 0;
    /* ctx >= limit * pct/100, kept in integer math to avoid float. */
    return ctx_tokens * 100 >= limit * (long)pct;
}

/* Resolve the auto-compaction trigger point as a percentage of the context
 * window. The registry bounds it to 1..100, so an out-of-range value already
 * reads as the registry default. */
static int threshold_pct(void)
{
    return config_int("compact.threshold");
}

int compact_should_auto(long ctx_tokens, long limit)
{
    if (!compact_auto_enabled())
        return 0;
    return compact_over_threshold(ctx_tokens, limit, threshold_pct());
}

long compact_context_limit(const struct provider *p, const char *model)
{
    long env = config_size("context_limit");
    if (env > 0)
        return env;
    long auto_v = atomic_load(&p->context_limit);
    if (auto_v > 0)
        return auto_v;
    if (p->catalog_id && model && *model) {
        struct catalog_entry e;
        catalog_lookup(p->catalog_id, model, &e);
        if (e.context > 0)
            return e.context;
    }
    return 0;
}

static char *compact_build_prompt(const char *instructions)
{
    if (instructions && *instructions)
        return xasprintf("%s\n\nAdditional focus for this summary:\n%s", COMPACT_PROMPT,
                         instructions);
    return xstrdup(COMPACT_PROMPT);
}

static char *compact_build_seed(const char *summary)
{
    return xasprintf("%s\n\n%s", COMPACT_SEED_PREAMBLE, summary);
}

static char *compact_extract_summary(const struct item *items, size_t n)
{
    const char *found = NULL;
    for (size_t i = 0; i < n; i++) {
        if (items[i].kind == ITEM_ASSISTANT_MESSAGE && items[i].text && items[i].text[0])
            found = items[i].text;
    }
    return found ? xstrdup(found) : NULL;
}

#define COMPACT_ATTEMPTS_MAX 8
struct compact_usage {
    struct {
        struct stream_usage u;
        long ms;
    } att[COMPACT_ATTEMPTS_MAX];
    int n;
    long t0;
};

static void compact_usage_init(struct compact_usage *cu)
{
    cu->n = 0;
    cu->t0 = monotonic_ms();
}

static void compact_usage_record(struct compact_usage *cu, const struct stream_usage *u)
{
    long now = monotonic_ms();
    if (cu->n < COMPACT_ATTEMPTS_MAX) {
        cu->att[cu->n].u = *u;
        cu->att[cu->n].ms = now - cu->t0;
        cu->n++;
    }
    cu->t0 = now;
}

/* initial summarization stream + up to 3 retries that reject a tool call. */
#define COMPACT_MAX_ATTEMPTS 4

/* Synthetic tool_result fed back when a model ignores the "text only"
 * instruction and answers with a tool call. We never execute a tool while
 * summarizing — that would mutate state mid-condense — so the call is rejected
 * and the model nudged back to text. */
static const char COMPACT_REJECT_MSG[] =
    "[rejected] Tool calls are disabled while summarizing. Respond with the summary text "
    "only — do not call any tools.";

/* Grow `*req` by one, transferring ownership of `it` into the vector. */
static void req_push(struct item **req, size_t *n, size_t *cap, struct item it)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *req = xrealloc(*req, *cap * sizeof(**req));
    }
    (*req)[(*n)++] = it;
}

static char *compact_summarize(const struct agent_session *s, struct provider *p,
                               const char *instructions, struct turn *t, stream_cb cb,
                               void *cb_user, http_tick_cb tick, void *tick_user, int *attempts)
{
    /* The request is the live history plus a synthetic trailing user message
     * carrying the summarization prompt. Tools ARE advertised so the cached
     * prefix the live conversation built (tools → system → messages) stays
     * warm for this whole-history read; the prompt tells the model to answer
     * in text only. A weaker model may ignore that and emit a tool call
     * instead — we then append a rejected tool_result and stream again
     * (cache-warm: only a tiny suffix is new each retry), capped so a stubborn
     * model can't loop forever.
     *
     * req[0..base) are shallow copies of the session's items (strings borrowed
     * — never item_free'd); req[base] is the prompt slot (its text is `prompt`,
     * freed once at the end, not via item_free); anything appended past that is
     * owned by req and item_free'd on cleanup. */
    char *prompt = compact_build_prompt(instructions);
    size_t base = s->n_items;
    size_t cap = base + 1;
    struct item *req = xmalloc(cap * sizeof(*req));
    memcpy(req, s->items, base * sizeof(*req));
    req[base] = (struct item){.kind = ITEM_USER_MESSAGE, .text = prompt};
    size_t n = base + 1;

    char *summary = NULL;
    for (int attempt = 0; attempt < COMPACT_MAX_ATTEMPTS; attempt++) {
        struct context ctx = {
            .system_prompt = s->sys,
            .items = req,
            .n_items = n,
            .tools = s->tools,
            .n_tools = s->n_tools,
            .effort = s->effort,
        };
        if (attempts)
            (*attempts)++;
        p->stream(p, &ctx, s->model, cb, cb_user, tick, tick_user);

        /* A stream error leaves the turn holding partial items for the
         * caller's turn_reset to free; don't extract from a half-formed turn. */
        if (t->error)
            break;

        size_t got_n;
        struct item *got = turn_take_items(t, &got_n);

        int had_call = 0;
        for (size_t i = 0; i < got_n; i++)
            if (got[i].kind == ITEM_TOOL_CALL) {
                had_call = 1;
                break;
            }

        /* Accept only a tool-free response as the checkpoint. A tool call —
         * even alongside a text preamble that turn_on_event already flushed
         * into an assistant message ("I'll inspect the files" then read) —
         * means the model ignored "text only": never keep that preamble as the
         * summary and never run the tool. With no call, take the summary and
         * stop; otherwise reject each call and re-stream, giving up with no
         * summary once out of attempts. */
        if (!had_call)
            summary = compact_extract_summary(got, got_n);
        if (!had_call || attempt == COMPACT_MAX_ATTEMPTS - 1) {
            for (size_t i = 0; i < got_n; i++)
                item_free(&got[i]);
            free(got);
            break;
        }

        /* Append the assistant turn verbatim (ownership transfers into req),
         * then a rejected tool_result for each tool call it made, and loop. */
        size_t start = n;
        for (size_t i = 0; i < got_n; i++)
            req_push(&req, &n, &cap, got[i]);
        free(got); /* items moved; free only the array */
        size_t end = n;
        for (size_t i = start; i < end; i++) {
            if (req[i].kind != ITEM_TOOL_CALL)
                continue;
            char *cid = req[i].call_id; /* read before req_push may realloc */
            req_push(&req, &n, &cap,
                     (struct item){.kind = ITEM_TOOL_RESULT,
                                   .call_id = cid ? xstrdup(cid) : NULL,
                                   .output = xstrdup(COMPACT_REJECT_MSG)});
        }
        turn_reset(t); /* clear pending/bufs before the next stream */
    }

    for (size_t i = base + 1; i < n; i++)
        item_free(&req[i]);
    free(req);
    free(prompt);
    return summary;
}

static void compact_apply(struct agent_session *s, struct session_log *slog,
                          struct transcript_log *tlog, const char *summary)
{
    char *seed = compact_build_seed(summary);
    agent_session_reset(s);
    items_append(&s->items, &s->n_items, &s->cap_items,
                 (struct item){.kind = ITEM_USER_MESSAGE, .text = seed, .compact_seed = 1});
    /* Rotate both logs to fresh files, then write the seed. The old records
     * remain on disk for archaeology. */
    session_log_reset(slog);
    transcript_log_reset(tlog, s->sys, s->tools, s->n_tools);
    transcript_log_append(tlog, s->items, s->n_items);
    session_log_append(slog, s->items, s->n_items);
    /* Bash temp files referenced by the discarded turns are now unreachable. */
    bash_cleanup_tempfiles();
}

struct compact_sink {
    struct turn assembly;
    struct compact_usage usage;
    const struct compact_hooks *hooks;
    char *error_message;
};

static int compact_sink_on_event(const struct stream_event *ev, void *user)
{
    struct compact_sink *sink = user;
    const struct stream_usage *usage = NULL;

    if (ev->kind == EV_DONE) {
        usage = &ev->u.done.usage;
    } else if (ev->kind == EV_ERROR) {
        if (!sink->error_message)
            sink->error_message =
                xstrdup(ev->u.error.message ? ev->u.error.message : "stream failed");
        usage = ev->u.error.usage;
    }
    if (usage)
        compact_usage_record(&sink->usage, usage);
    if (sink->hooks->observe)
        sink->hooks->observe(ev, sink->hooks->user);
    turn_on_event(ev, &sink->assembly);
    return 0;
}

static void compact_flush_logs(const struct compact_params *params)
{
    transcript_log_append(params->tlog, params->session->items, params->session->n_items);
    session_log_append(params->slog, params->session->items, params->session->n_items);
}

static void compact_add_usage(const struct compact_params *params,
                              const struct compact_usage *usage, int from, int to)
{
    for (int i = from; i < to; i++)
        agent_session_add_turn_usage(params->session, params->provider, &usage->att[i].u,
                                     usage->att[i].ms);
}

void compact_run(const struct compact_params *params, struct compact_result *result)
{
    memset(result, 0, sizeof(*result));
    if (!params->provider) {
        result->outcome = COMPACT_NO_PROVIDER;
        return;
    }
    if (!params->session->model || !params->session->model[0]) {
        result->outcome = COMPACT_NO_MODEL;
        return;
    }
    if (params->session->n_items == 0) {
        result->outcome = COMPACT_EMPTY;
        return;
    }

    struct compact_sink sink = {.hooks = &params->hooks};
    turn_init(&sink.assembly);
    compact_usage_init(&sink.usage);
    char *summary = compact_summarize(params->session, params->provider, params->instructions,
                                      &sink.assembly, compact_sink_on_event, &sink,
                                      params->hooks.tick, params->hooks.user, &result->attempts);
    int cancelled = params->hooks.cancelled && params->hooks.cancelled(params->hooks.user);
    if (cancelled) {
        free(summary);
        summary = NULL;
    }
    turn_reset(&sink.assembly);
    result->error_message = sink.error_message;

    if (!summary || !summary[0]) {
        compact_add_usage(params, &sink.usage, 0, sink.usage.n);
        compact_flush_logs(params);
        free(summary);
        if (cancelled)
            result->outcome = COMPACT_CANCELLED;
        else if (result->error_message)
            result->outcome = COMPACT_PROVIDER_ERROR;
        else
            result->outcome = COMPACT_NO_SUMMARY;
        return;
    }

    /* Rejected attempts belong to the old history; the accepted attempt's
     * footer follows the compact seed in the rotated logs. */
    int accepted = sink.usage.n > 0 ? sink.usage.n - 1 : 0;
    compact_add_usage(params, &sink.usage, 0, accepted);
    compact_flush_logs(params);
    compact_apply(params->session, params->slog, params->tlog, summary);
    free(summary);
    compact_add_usage(params, &sink.usage, accepted, sink.usage.n);
    compact_flush_logs(params);
    result->outcome = COMPACT_COMPLETE;
}

void compact_result_destroy(struct compact_result *result)
{
    free(result->error_message);
    result->error_message = NULL;
}
