/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_RENDER_CTX_H
#define HAX_RENDER_RENDER_CTX_H

#include <stddef.h>

#include "render/disp.h"

struct spinner;
struct md_renderer;

enum render_mode {
    RENDER_IDLE = 0,
    RENDER_WAITING,
    RENDER_REASONING,
    RENDER_TEXT,
    RENDER_TOOL_CLUSTER, /* collapsed tool calls; persists across stream boundaries */
};

/* Live terminal rendering state. The context does not own spinner or md. */
struct render_ctx {
    enum render_mode mode;
    struct disp disp;
    struct spinner *spinner;
    struct md_renderer *md; /* NULL disables Markdown rendering */

    struct {
        const char *last_tool; /* borrowed registry-static name */
        int line_open;
        int line_cells; /* also the parked spinner's cursor-restore column */
    } cluster;

    struct {
        long started_at_ms; /* 0 when no buffered table is being timed */
        int spinner_visible;
    } table;

    struct {
        long deadline_ms; /* 0 when no retry is pending */
        int next_attempt;
        int max_attempts;
    } retry;

    struct {
        int content_seen; /* the current stream has content a pause must preserve */
        int answer_started;
    } stream;

    int show_reasoning;
};

/* Close the current mode and enter target, releasing its cursor and style state. */
void render_set_mode(struct render_ctx *render, enum render_mode target);

/* End the current streamed item and show the waiting indicator. Tool clusters remain open. */
void render_stream_boundary(struct render_ctx *render);

/* Reset per-stream state and Markdown, then enter waiting unless a tool cluster remains open. */
void render_stream_begin(struct render_ctx *render);

/* Close the current mode and separate the next out-of-band visual block. */
void render_open_block(struct render_ctx *render);

/* Write model text in the current text or reasoning mode. */
void render_write_text(struct render_ctx *render, const char *bytes, size_t len);

/* Render assistant text, ignoring initial line endings until visible text arrives. */
void render_text_delta(struct render_ctx *render, const char *bytes, size_t len);

/* Show the delayed spinner for a buffered table. No-op outside text mode. */
void render_show_table_spinner(struct render_ctx *render);

/* Repaint the retry spinner label from the current retry state. */
void render_update_retry_label(struct render_ctx *render);

#endif /* HAX_RENDER_RENDER_CTX_H */
