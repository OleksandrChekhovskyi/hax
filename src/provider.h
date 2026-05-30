/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDER_H
#define HAX_PROVIDER_H

#include <stdatomic.h>
#include <stddef.h>

#include "transport/http.h"

/*
 * A flat, provider-agnostic view of a conversation. Each item is one thing:
 * a user message, an assistant message, a tool call, or a tool result.
 * This maps cleanly to the OpenAI Responses API "input" array, and with a
 * small amount of grouping in the adapter, to Anthropic Messages too.
 */
enum item_kind {
    ITEM_USER_MESSAGE,
    ITEM_ASSISTANT_MESSAGE,
    ITEM_TOOL_CALL,
    ITEM_TOOL_RESULT,
    /* Reasoning carried across turns. Two flavors share this kind and the
     * reasoning_* fields on struct item: the Codex Responses adapter emits
     * an opaque encrypted blob (reasoning_json) for chain-of-thought
     * continuity, while the openai-family adapters capture plain CoT text
     * (reasoning_text) to replay as reasoning_content for interleaved-
     * thinking models. Adapters that need neither simply skip this kind. */
    ITEM_REASONING,
    /* Inert marker the agent emits before each fresh model request, so
     * downstream consumers (currently the transcript renderer) can mark
     * turn boundaries the same way agent.c/turn.c sees them — one per
     * HTTP round-trip — without re-deriving them by heuristics. Carries
     * no fields. Provider adapters ignore it when serializing the
     * conversation. */
    ITEM_TURN_BOUNDARY,
};

struct item {
    enum item_kind kind;
    /* USER_MESSAGE / ASSISTANT_MESSAGE: */
    char *text;
    /* TOOL_CALL / TOOL_RESULT: */
    char *call_id;
    /* TOOL_CALL: */
    char *tool_name;
    char *tool_arguments_json;
    /* TOOL_RESULT: */
    char *output;
    /* REASONING round-trip form (Codex): full JSON object (e.g.
     * {"type":"reasoning","id":...,"summary":[...],"encrypted_content":...})
     * ready to be re-sent. This is what the model needs back. */
    char *reasoning_json;
    /* REASONING human-readable form: plain chain-of-thought text. For the
     * openai-family it arrives via `reasoning_content`/`reasoning` and is
     * round-tripped as reasoning_content when the provider opts in (see
     * openai_preset.roundtrip_reasoning_field). For Codex it is the streamed
     * summary, carried alongside reasoning_json purely for display (the JSON
     * is what gets re-sent). An item may have either or both; the transcript
     * prefers this text and falls back to the opaque reasoning_json tag. */
    char *reasoning_text;
};

void item_free(struct item *it);

struct tool_def {
    const char *name;
    const char *description;
    const char *parameters_schema_json; /* literal JSON Schema */
    const char *display_arg;            /* name of the JSON field shown after "[tool]" in the UI */
};

struct context {
    const char *system_prompt;
    const struct item *items;
    size_t n_items;
    const struct tool_def *tools;
    size_t n_tools;
    /* Optional. When non-NULL, providers pass it verbatim to whichever
     * field their API uses (Responses: reasoning.effort, Chat Completions:
     * reasoning_effort). NULL means "don't send" so the server picks its
     * own default. Values like "minimal"/"low"/"medium"/"high"/"xhigh" are
     * passed through unchanged — hax doesn't validate or clamp. */
    const char *reasoning_effort;
};

/* Token accounting for one completed response. -1 means "not reported by
 * this provider/backend" — many OpenAI-compatible servers don't surface
 * cached_tokens, and some omit usage entirely. Callers must check for -1
 * before formatting. input_tokens is everything we just sent (system +
 * full history + the new user message); output_tokens is what was just generated.
 * "context used" for display = input + output, since both will be in the
 * next request's input. cached_tokens is a subset of input_tokens (prefix
 * cache hit) — informational, not additive. */
struct stream_usage {
    long input_tokens;
    long output_tokens;
    long cached_tokens;
};

/* Events emitted by a provider's stream() into a stream_cb. */
enum stream_event_kind {
    EV_TEXT_DELTA,
    EV_TOOL_CALL_START, /* id + name known */
    EV_TOOL_CALL_DELTA, /* partial JSON args */
    EV_TOOL_CALL_END,   /* args finalized */
    EV_REASONING_ITEM,  /* opaque provider blob to round-trip on next turn */
    /* The model is currently producing reasoning/thinking tokens. Carries
     * the delta text (may be NULL/empty if the provider only signals the
     * state without exposing content, e.g. OpenAI o-series via Responses
     * API — such state-only deltas are ignored). Drives the "thinking..."
     * spinner, and the text is accumulated into an ITEM_REASONING
     * (reasoning_text). For opted-in openai-family providers that text is
     * replayed to the model as reasoning_content (see
     * openai_preset.roundtrip_reasoning_field); Codex instead round-trips
     * an opaque blob via EV_REASONING_ITEM and keeps the delta text only
     * for the transcript. */
    EV_REASONING_DELTA,
    /* A streaming HTTP attempt failed with a transient error and the
     * provider is about to retry. Pure UX signal — the agent uses it
     * to update the spinner label so the user sees that we're not
     * stuck on a dead connection. The eventual outcome (success or
     * EV_ERROR after exhausting retries) arrives later. */
    EV_RETRY,
    /* Prompt-processing (prefill) progress for this turn. Carries
     * llama.cpp-style counts: `processed`/`total` are tokens, `cache`
     * is the prefix-cache hit subset of `total` (so cache <=
     * processed <= total). The UX-meaningful fraction is
     * (processed - cache) / (total - cache), which starts at 0% each
     * turn regardless of cache reuse. Pure UX signal — never committed
     * to history. Only llama.cpp currently sources these; the openai
     * translation gates emission behind a per-preset flag (set
     * return_progress:true in the request body) so backends that don't
     * speak it never see synthesized events. */
    EV_PROGRESS,
    EV_DONE,
    EV_ERROR,
};

struct stream_event {
    enum stream_event_kind kind;
    union {
        struct {
            const char *text;
        } text_delta;
        struct {
            const char *id;
            const char *name;
        } tool_call_start;
        struct {
            const char *id;
            const char *args_delta;
        } tool_call_delta;
        struct {
            const char *id;
        } tool_call_end;
        struct {
            const char *json;
        } reasoning_item;
        struct {
            const char *text; /* NULL or "" allowed — signals reasoning
                                 activity even when no plaintext is exposed */
        } reasoning_delta;
        struct {
            int attempt;      /* 1-based attempt that just failed */
            int max_attempts; /* total attempts the provider will make */
            long delay_ms;    /* about to sleep this long before retrying */
            int http_status;  /* 0 = transport error, otherwise HTTP code */
        } retry;
        struct {
            long processed; /* tokens prefilled so far this turn */
            long total;     /* total prompt tokens to prefill */
            long cache;     /* subset of `total` served from prefix cache */
        } progress;
        struct {
            const char *stop_reason;
            struct stream_usage usage;
        } done;
        struct {
            const char *message;
            int http_status;
        } error;
    } u;
};

typedef int (*stream_cb)(const struct stream_event *ev, void *user);

struct provider {
    const char *name;
    /* Model id used when HAX_MODEL is unset. NULL = no safe default; the
     * agent will refuse to start and print an error. */
    const char *default_model;
    /* Reasoning effort used when HAX_REASONING_EFFORT is unset. NULL means
     * omit the field and let the backend choose. */
    const char *default_reasoning_effort;
    /* Stream a model response. The provider drives the HTTP round-trip
     * and translates SSE events into stream_event callbacks (`cb`). The
     * `tick` slot is the agent's side-channel hook into the wait loop —
     * called periodically (~1Hz) and on each received chunk, with the
     * agent's user pointer. Returning non-zero aborts the transfer.
     * Providers thread it straight through to http_sse_post; the mock
     * provider polls it from its sleep loop so scripted pauses exercise
     * the same path. NULL tick disables the hook. */
    int (*stream)(struct provider *p, const struct context *ctx, const char *model, stream_cb cb,
                  void *user, http_tick_cb tick, void *tick_user);
    /* Optional. Print a provider-specific subscription/usage report to
     * stdout (rate-limit windows for a Codex-style plan, paid-API spend
     * totals, etc.). NULL means "/usage is not supported on this
     * provider". Returns 0 on success, -1 on fetch/parse failure (the
     * implementation is expected to have already printed a diagnostic
     * to stderr in that case). */
    int (*query_usage)(struct provider *p);
    void (*destroy)(struct provider *p);
    /* Auto-discovered model context window in tokens. 0 = unknown (no
     * probe ran, hasn't completed yet, or failed); positive values are
     * what the agent's status line uses to render the "%" of context
     * used. Updated atomically — provider construction may set it
     * synchronously from HAX_CONTEXT_LIMIT, or a background probe owned
     * by the implementation may write it once the catalog response
     * arrives. The agent reads it on every usage update, so a
     * late-landing probe fills in the percentage starting with
     * whichever turn it completes during. Implementations own the
     * worker that writes here and are responsible for joining it in
     * their destroy() before any teardown that could free this slot. */
    _Atomic long context_limit;
};

/* Static provider descriptor. Each provider .c file defines exactly one
 * `const struct provider_factory PROVIDER_<NAME>` symbol; main.c collects
 * them into a registry and matches HAX_PROVIDER against `name`. The
 * default when HAX_PROVIDER is unset lives in main.c's DEFAULT_PROVIDER
 * constant, decoupled from registry order. */
struct provider_factory {
    const char *name; /* HAX_PROVIDER value, e.g. "codex", "llama.cpp" */
    struct provider *(*new)(void);
};

#endif /* HAX_PROVIDER_H */
