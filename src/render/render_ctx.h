/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_RENDER_CTX_H
#define HAX_RENDER_RENDER_CTX_H

#include <stddef.h>

#include "render/disp.h"

struct footer;
struct md_renderer;

enum render_mode {
    RENDER_IDLE = 0,
    RENDER_WAITING,
    RENDER_REASONING,
    RENDER_TEXT,
    RENDER_TOOL_CLUSTER, /* collapsed tool calls; persists across stream boundaries */
    RENDER_TOOL,         /* a verbose tool call, which draws its own block */
};

/* Live terminal rendering state. The context does not own footer or md. */
struct render_ctx {
    enum render_mode mode;
    struct disp disp;
    struct footer *footer;
    struct md_renderer *md; /* NULL disables Markdown rendering */

    struct {
        const char *last_tool; /* borrowed registry-static name */
        int line_open;         /* a read row awaiting more reads; disp.column is its width */
    } cluster;

    struct {
        long deadline_ms; /* 0 when no retry is pending */
        int next_attempt;
        int max_attempts;
    } retry;

    struct {
        int content_seen; /* the current stream has content a pause must preserve */
        int answer_started;
        int output_rendered; /* the current attempt wrote text or reasoning to the terminal */
    } stream;

    int show_reasoning;
};

/* Close the current mode and enter target, releasing its cursor and style state. */
void render_set_mode(struct render_ctx *render, enum render_mode target);

/* End the current streamed item and wait for the next one. Tool clusters remain open. */
void render_stream_boundary(struct render_ctx *render);

/* Reset per-stream state and Markdown, enter waiting unless a tool cluster remains open, and show
 * the status row. The row stays up through every mode until render_finish(). */
void render_stream_begin(struct render_ctx *render);

/* Close the current mode, hide the status row, and reset its label: no more work follows. */
void render_finish(struct render_ctx *render);

/* Settle a partially rendered attempt before a retry: mark the cut where output was already
 * visible, reset per-stream state so the retried attempt renders fresh, and hold the retry label
 * until render_retry_end(). failed_attempt is 1-based. */
void render_stream_retry(struct render_ctx *render, int failed_attempt, int max_attempts,
                         long delay_ms);

/* The retried attempt produced its first event: release the retry label. A no-op without a
 * pending retry. */
void render_retry_end(struct render_ctx *render);

/* Close the current mode and separate the next out-of-band visual block. */
void render_open_block(struct render_ctx *render);

/* Write model text in the current text or reasoning mode. */
void render_write_text(struct render_ctx *render, const char *bytes, size_t len);

/* Render assistant text, ignoring initial line endings until visible text arrives. */
void render_text_delta(struct render_ctx *render, const char *bytes, size_t len);

/* Report reasoning, rendering its text only when reasoning is shown. text may be NULL. */
void render_reasoning_delta(struct render_ctx *render, const char *text);

/* End a completed reasoning item. */
void render_reasoning_end(struct render_ctx *render);

/* Report a tool call whose arguments stream unseen until dispatch. tool_name may be NULL or
 * empty. */
void render_tool_call_start(struct render_ctx *render, const char *tool_name);

/* Report a dispatched tool call running. */
void render_tool_running(struct render_ctx *render, const char *tool_name);

/* Report prompt prefill progress in tokens, of which cached ones are served without work. */
void render_progress(struct render_ctx *render, long processed, long total, long cached);

/* Format the retry indicator for now_ms into out: a countdown while the retry sleep is pending,
 * and a plain attempt marker once the deadline has passed and the retried request is in flight. */
void render_retry_label(const struct render_ctx *render, long now_ms, char *out, size_t size);

/* Repaint the retry label from the current retry state; a no-op without a pending retry. */
void render_update_retry_label(struct render_ctx *render);

#endif /* HAX_RENDER_RENDER_CTX_H */
