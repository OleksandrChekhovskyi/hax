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

/* Events emitted by a provider's stream() into a stream_cb. */
enum stream_event_kind {
    EV_TEXT_DELTA,
    EV_TOOL_CALL_START, /* id + name known */
    EV_TOOL_CALL_DELTA, /* partial JSON args */
    EV_TOOL_CALL_END,   /* args finalized */
    EV_REASONING_ITEM,  /* opaque provider blob to round-trip on next turn */
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
            const char *stop_reason;
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
    int (*stream)(struct provider *p, const struct context *ctx, const char *model, stream_cb cb,
                  void *user);
    void (*destroy)(struct provider *p);
};

#endif /* HAX_PROVIDER_H */
