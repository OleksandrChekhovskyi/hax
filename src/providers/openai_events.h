/* SPDX-License-Identifier: MIT */
#ifndef HAX_OPENAI_EVENTS_H
#define HAX_OPENAI_EVENTS_H

#include <stddef.h>

#include "provider.h"
#include "util.h"

/*
 * Parses OpenAI Chat Completions streaming chunks and translates them into
 * provider-agnostic stream_event callbacks. Tool calls are streamed as a
 * per-choice array keyed by `index`; we track per-index state so we can emit
 * EV_TOOL_CALL_START exactly once (when both id and name are known) and
 * EV_TOOL_CALL_END on finish_reason.
 *
 * Only the modern `tool_calls` shape is handled — the legacy pre-2023
 * `function_call` / `finish_reason: "function_call"` format is out of scope,
 * since every current OpenAI-compatible backend we target emits `tool_calls`.
 */

struct openai_tool_track {
    int index;
    char *id;
    char *name;
    /* Argument bytes that arrived before EV_TOOL_CALL_START could be
     * emitted (i.e. before both id and name were known). Flushed as a
     * single DELTA the moment START fires. */
    struct buf pending_args;
    int start_emitted; /* EV_TOOL_CALL_START already sent */
    int ended;         /* EV_TOOL_CALL_END already sent */
};

struct openai_events {
    stream_cb cb;
    void *user;

    struct openai_tool_track *tools;
    size_t n_tools;
    size_t cap_tools;

    /* finish_reason arrives one chunk before the optional usage chunk and
     * before [DONE], so EV_DONE has to be deferred to bundle usage with it.
     * saw_finish: a non-error finish_reason was received; reason is saved.
     * pending_usage: harvested from any chunk that carries `usage` (the
     * trailing chunk under stream_options.include_usage; may be -1/-1/-1
     * if the backend never sends one). */
    int saw_finish;
    char *finish_reason;
    struct stream_usage pending_usage;

    int terminated; /* any terminal event (EV_DONE or EV_ERROR) emitted */

    /* When set, surface top-level `prompt_progress` chunks (llama.cpp's
     * return_progress=true) as EV_PROGRESS. Off by default — real OpenAI
     * and most compat backends never send the field, but a stray match
     * would still parse, so keep emission opt-in. */
    int emit_progress;
};

void openai_events_init(struct openai_events *s, stream_cb cb, void *user);
void openai_events_free(struct openai_events *s);

/* Feed one SSE `data:` payload (the JSON chunk, or "[DONE]"). */
void openai_events_feed(struct openai_events *s, const char *data);

/* Emit EV_ERROR("stream ended before completion") if no terminal event was
 * produced. Call once the SSE transport closes cleanly. */
void openai_events_finalize(struct openai_events *s);

#endif /* HAX_OPENAI_EVENTS_H */
