/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDER_H
#define HAX_PROVIDER_H

#include <stddef.h>

/*
 * A flat, provider-agnostic view of a conversation. Each item is one thing:
 * a user turn, an assistant text turn, a tool call, or a tool result.
 * This maps cleanly to the OpenAI Responses API "input" array, and with a
 * small amount of grouping in the adapter, to Anthropic Messages too.
 */
enum item_kind {
    ITEM_USER_MESSAGE,
    ITEM_ASSISTANT_MESSAGE,
    ITEM_TOOL_CALL,
    ITEM_TOOL_RESULT,
    /* Provider-specific blob carried verbatim across turns. Currently only
     * the Codex Responses adapter produces these (encrypted reasoning items
     * for chain-of-thought continuity); other providers ignore them. */
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
    /* REASONING: full JSON object (e.g. {"type":"reasoning","id":...,
     * "summary":[...],"encrypted_content":"..."}) ready to be re-sent. */
    char *reasoning_json;
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
 * full history + new user turn); output_tokens is what was just generated.
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
     * the delta text (may be NULL if the provider only signals the state
     * without exposing content, e.g. OpenAI o-series via Responses API).
     * Drives UX only — the agent doesn't store this in history; reasoning
     * round-trip goes through EV_REASONING_ITEM. */
    EV_REASONING_DELTA,
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
    int (*stream)(struct provider *p, const struct context *ctx, const char *model, stream_cb cb,
                  void *user);
    void (*destroy)(struct provider *p);
};

/* Static provider descriptor. Each provider .c file defines exactly one
 * `const struct provider_factory PROVIDER_<NAME>` symbol; main.c collects
 * them into a registry and matches HAX_PROVIDER against `name`. The first
 * entry in the registry array is the default when HAX_PROVIDER is unset. */
struct provider_factory {
    const char *name; /* HAX_PROVIDER value, e.g. "codex", "llama.cpp" */
    struct provider *(*new)(void);
};

#endif /* HAX_PROVIDER_H */
