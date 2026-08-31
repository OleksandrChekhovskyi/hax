/* SPDX-License-Identifier: MIT */
#include "render/render_ctx.h"

#include <stdio.h>

#include "render/disp.h"
#include "render/markdown.h"
#include "render/spinner.h"
#include "system/clock.h"
#include "terminal/ansi.h"

static int hide_table_spinner(struct render_ctx *render)
{
    if (!render->table.spinner_visible)
        return 0;
    spinner_hide(render->spinner);
    render->table.spinner_visible = 0;
    return 1;
}

static void close_mode(struct render_ctx *render)
{
    switch (render->mode) {
    case RENDER_IDLE:
        break;
    case RENDER_WAITING:
        spinner_hide(render->spinner);
        break;
    case RENDER_REASONING:
        if (render->md)
            md_flush(render->md);
        disp_write_ansi(&render->disp, ANSI_RESET);
        disp_putc(&render->disp, '\n');
        break;
    case RENDER_TEXT:
        if (render->md)
            md_flush(render->md);
        break;
    case RENDER_TOOL_CLUSTER:
        /* Hide first because a parked spinner restores the cluster cursor position. */
        spinner_hide(render->spinner);
        if (render->cluster.line_open)
            disp_putc(&render->disp, '\n');
        disp_flush(&render->disp);
        render->cluster.last_tool = NULL;
        render->cluster.line_open = 0;
        render->cluster.line_cells = 0;
        break;
    }
}

static void open_mode(struct render_ctx *render, enum render_mode mode)
{
    switch (mode) {
    case RENDER_IDLE:
        break;
    case RENDER_WAITING:
        disp_block_separator(&render->disp);
        spinner_set_label(render->spinner, "working", "working...");
        spinner_show(render->spinner);
        break;
    case RENDER_REASONING:
        spinner_hide(render->spinner);
        disp_block_separator(&render->disp);
        disp_write_ansi(&render->disp, ANSI_DIM ANSI_ITALIC);
        /* Markdown styling would override the surrounding reasoning style. */
        if (render->md)
            md_set_styled(render->md, 0);
        break;
    case RENDER_TEXT:
        spinner_hide(render->spinner);
        disp_block_separator(&render->disp);
        render->stream.answer_started = 1;
        if (render->md)
            md_set_styled(render->md, 1);
        break;
    case RENDER_TOOL_CLUSTER:
        spinner_hide(render->spinner);
        disp_block_separator(&render->disp);
        /* The parked row must not inherit a label from the previous mode. */
        spinner_set_label(render->spinner, "working", "working...");
        break;
    }
}

void render_set_mode(struct render_ctx *render, enum render_mode target)
{
    if (render->mode == target)
        return;

    /* Closing Markdown may render into the composing spinner's row. */
    hide_table_spinner(render);
    close_mode(render);
    open_mode(render, target);
    render->mode = target;
}

static void render_wait_for_stream(struct render_ctx *render)
{
    render->table.started_at_ms = 0;
    render_set_mode(render, RENDER_WAITING);
    /* Deferred labels prevent rapid item boundaries from flickering. */
    spinner_request_label(render->spinner, "working", "working...");
    spinner_show(render->spinner);
}

void render_stream_boundary(struct render_ctx *render)
{
    if (render->mode == RENDER_TOOL_CLUSTER) {
        spinner_request_label(render->spinner, "working", "working...");
        return;
    }
    render_wait_for_stream(render);
}

void render_stream_begin(struct render_ctx *render)
{
    render->table.started_at_ms = 0;
    render->retry.deadline_ms = 0;
    render->retry.next_attempt = 0;
    render->retry.max_attempts = 0;
    render->stream.content_seen = 0;
    render->stream.answer_started = 0;
    render->stream.output_rendered = 0;

    if (render->mode != RENDER_TOOL_CLUSTER)
        render_wait_for_stream(render);
    if (render->md)
        md_reset(render->md, md_cols());
}

void render_stream_retry(struct render_ctx *render)
{
    if (render->stream.output_rendered) {
        render_open_block(render);
        disp_write_ansi(&render->disp, ANSI_DIM);
        disp_printf(&render->disp, "[unexpected end]");
        disp_write_ansi(&render->disp, ANSI_RESET);
        disp_putc(&render->disp, '\n');
        render_set_mode(render, RENDER_WAITING);
    }
    render->table.started_at_ms = 0;
    render->stream.content_seen = 0;
    render->stream.answer_started = 0;
    render->stream.output_rendered = 0;
    if (render->md)
        md_reset(render->md, md_cols());
}

void render_open_block(struct render_ctx *render)
{
    render_set_mode(render, RENDER_IDLE);
    /* Tool-status rows are spinner-owned but are not represented by render_mode. */
    spinner_hide(render->spinner);
    disp_block_separator(&render->disp);
}

void render_show_table_spinner(struct render_ctx *render)
{
    if (render->table.spinner_visible || render->mode != RENDER_TEXT)
        return;

    /* The spinner's erase-line sequence must land below already-rendered text. */
    disp_commit_newlines(&render->disp);
    spinner_set_label(render->spinner, "composing", "composing...");
    spinner_show(render->spinner);
    render->table.spinner_visible = 1;
}

void render_write_text(struct render_ctx *render, const char *bytes, size_t len)
{
    render->stream.output_rendered = 1;
    /* A feed may complete the table and render into the spinner's row. */
    int spinner_was_visible = hide_table_spinner(render);
    int was_in_table = render->md && md_in_table(render->md);

    if (render->md)
        md_feed(render->md, bytes, len);
    else
        disp_write(&render->disp, bytes, len);
    disp_flush(&render->disp);

    if (render->md && md_in_table(render->md)) {
        if (!was_in_table)
            render->table.started_at_ms = monotonic_ms();
        if (spinner_was_visible)
            render_show_table_spinner(render);
    } else {
        render->table.started_at_ms = 0;
    }
}

void render_text_delta(struct render_ctx *render, const char *bytes, size_t len)
{
    if (!render->stream.answer_started) {
        while (len > 0 && (*bytes == '\n' || *bytes == '\r')) {
            bytes++;
            len--;
        }
    }
    if (len == 0)
        return;

    render_set_mode(render, RENDER_TEXT);
    render_write_text(render, bytes, len);
}

void render_update_retry_label(struct render_ctx *render)
{
    long remaining_ms = render->retry.deadline_ms - monotonic_ms();
    /* Round up so the final second is displayed as 1s rather than 0s. */
    long remaining_seconds = (remaining_ms + 999) / 1000;
    if (remaining_seconds < 1)
        remaining_seconds = 1;

    char label[64];
    snprintf(label, sizeof(label), "retrying in %lds (attempt %d/%d)...", remaining_seconds,
             render->retry.next_attempt, render->retry.max_attempts);
    spinner_set_label(render->spinner, "retry", label);
}
