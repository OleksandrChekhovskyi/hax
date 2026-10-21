/* SPDX-License-Identifier: MIT */
#include "render/render_ctx.h"

#include <stdio.h>
#include <string.h>

#include "render/disp.h"
#include "render/footer.h"
#include "render/markdown.h"
#include "system/clock.h"
#include "terminal/ansi.h"

static void close_mode(struct render_ctx *render)
{
    switch (render->mode) {
    case RENDER_IDLE:
        break;
    case RENDER_WAITING:
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
        if (render->cluster.line_open)
            disp_putc(&render->disp, '\n');
        disp_flush(&render->disp);
        render->cluster.last_tool = NULL;
        render->cluster.line_open = 0;
        break;
    case RENDER_TOOL:
        break;
    }
}

static void open_mode(struct render_ctx *render, enum render_mode mode)
{
    switch (mode) {
    case RENDER_IDLE:
    case RENDER_TOOL:
        break;
    case RENDER_WAITING:
    case RENDER_TOOL_CLUSTER:
        disp_block_separator(&render->disp);
        break;
    case RENDER_REASONING:
        disp_block_separator(&render->disp);
        disp_write_ansi(&render->disp, ANSI_DIM ANSI_ITALIC);
        /* Markdown styling would override the surrounding reasoning style. */
        if (render->md)
            md_set_styled(render->md, 0);
        break;
    case RENDER_TEXT:
        disp_block_separator(&render->disp);
        render->stream.answer_started = 1;
        if (render->md)
            md_set_styled(render->md, 1);
        break;
    }
}

void render_set_mode(struct render_ctx *render, enum render_mode target)
{
    if (render->mode == target)
        return;

    close_mode(render);
    open_mode(render, target);
    render->mode = target;
    disp_flush(&render->disp);
}

static void request_working(struct render_ctx *render)
{
    footer_request_label(render->footer, "working", "working...");
}

void render_stream_boundary(struct render_ctx *render)
{
    if (render->mode != RENDER_TOOL_CLUSTER)
        render_set_mode(render, RENDER_WAITING);
}

void render_stream_begin(struct render_ctx *render)
{
    render_retry_end(render);
    render->retry.next_attempt = 0;
    render->retry.max_attempts = 0;
    render->stream.content_seen = 0;
    render->stream.answer_started = 0;
    render->stream.output_rendered = 0;

    render_stream_boundary(render);
    if (render->md)
        md_reset(render->md, md_cols());
    /* A new request makes any earlier label stale. */
    request_working(render);
    footer_show(render->footer);
}

void render_finish(struct render_ctx *render)
{
    render_set_mode(render, RENDER_IDLE);
    footer_hide(render->footer);
    /* Hidden first, so the reset never paints. A held label must not outlive its work. */
    render->retry.deadline_ms = 0;
    footer_set_label(render->footer, "working", "working...");
}

void render_stream_retry(struct render_ctx *render, int failed_attempt, int max_attempts,
                         long delay_ms)
{
    if (render->stream.output_rendered) {
        render_open_block(render);
        disp_write_ansi(&render->disp, ANSI_DIM);
        disp_printf(&render->disp, "[unexpected end]");
        disp_write_ansi(&render->disp, ANSI_RESET);
        disp_putc(&render->disp, '\n');
        render_set_mode(render, RENDER_WAITING);
    }
    render->stream.content_seen = 0;
    render->stream.answer_started = 0;
    render->stream.output_rendered = 0;
    if (render->md)
        md_reset(render->md, md_cols());

    render->retry.deadline_ms = monotonic_ms() + delay_ms;
    render->retry.next_attempt = failed_attempt + 1;
    render->retry.max_attempts = max_attempts;
    render_update_retry_label(render);
}

void render_retry_end(struct render_ctx *render)
{
    if (!render->retry.deadline_ms)
        return;
    render->retry.deadline_ms = 0;
    footer_set_label(render->footer, "working", "working...");
}

void render_open_block(struct render_ctx *render)
{
    render_set_mode(render, RENDER_IDLE);
    disp_block_separator(&render->disp);
}

/* Expanded as verbatim Markdown output expands them: the terminal's own tab stops depend on the
 * screen width and clamp at its edge, which disp's column cannot follow. */
static void write_expanding_tabs(struct disp *disp, const char *bytes, size_t len)
{
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] != '\t')
            continue;
        disp_write(disp, bytes + start, i - start);
        disp_write(disp, "    ", 4);
        start = i + 1;
    }
    disp_write(disp, bytes + start, len - start);
}

/* Markdown is buffering a table, so nothing appears for a while. */
static int in_table(const struct render_ctx *render)
{
    return render->md && md_in_table(render->md);
}

void render_write_text(struct render_ctx *render, const char *bytes, size_t len)
{
    render->stream.output_rendered = 1;
    if (render->md)
        md_feed(render->md, bytes, len);
    else
        write_expanding_tabs(&render->disp, bytes, len);
    disp_flush(&render->disp);

    /* A table shows only once complete; the label explains the wait, and its settle time keeps a
     * quick table from flashing it. */
    if (in_table(render))
        footer_request_label(render->footer, "composing", "composing...");
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
    /* Streaming text shows its own progress; no other label still holds. */
    if (!in_table(render))
        request_working(render);
}

void render_reasoning_delta(struct render_ctx *render, const char *text)
{
    footer_request_label(render->footer, "thinking", "thinking...");
    if (!render->show_reasoning || !text || !*text)
        return;
    render_set_mode(render, RENDER_REASONING);
    render_write_text(render, text, strlen(text));
}

void render_reasoning_end(struct render_ctx *render)
{
    render_stream_boundary(render);
    request_working(render);
}

void render_tool_call_start(struct render_ctx *render, const char *tool_name)
{
    render_stream_boundary(render);
    if (!tool_name || !*tool_name) {
        request_working(render);
        return;
    }
    /* One key for every call keeps the label up across a batch, which supported providers stream
     * sequentially. */
    char label[64];
    snprintf(label, sizeof(label), "[%s] composing...", tool_name);
    footer_request_label(render->footer, "compose", label);
}

void render_tool_running(struct render_ctx *render, const char *tool_name)
{
    /* Keyed by tool, so a batch of calls to one tool keeps the label up. */
    char label[64];
    char key[32];
    snprintf(label, sizeof(label), "[%s] running...", tool_name);
    snprintf(key, sizeof(key), "run:%s", tool_name);
    footer_request_label(render->footer, key, label);
}

void render_progress(struct render_ctx *render, long processed, long total, long cached)
{
    /* Report uncached prefill work so progress starts at zero despite cache reuse. A fully cached
     * prompt has no work to report. */
    long uncached_total = total - cached;
    if (uncached_total <= 0)
        return;
    long uncached_processed = processed - cached;
    if (uncached_processed < 0)
        uncached_processed = 0;
    int percentage = (int)((uncached_processed * 100) / uncached_total);
    if (percentage < 0)
        percentage = 0;
    if (percentage > 100)
        percentage = 100;
    char label[32];
    snprintf(label, sizeof(label), "processing... %d%%", percentage);
    /* One key keeps the settling time while percentages refresh. */
    footer_request_label(render->footer, "processing", label);
}

void render_retry_label(const struct render_ctx *render, long now_ms, char *out, size_t size)
{
    long remaining_ms = render->retry.deadline_ms - now_ms;
    /* The retry state persists until the retried attempt produces its first event, so an
     * expired deadline means the request is in flight: a stalled attempt would otherwise read
     * as a countdown stuck at its final second. */
    if (remaining_ms <= 0) {
        snprintf(out, size, "retrying (attempt %d/%d)...", render->retry.next_attempt,
                 render->retry.max_attempts);
        return;
    }

    /* Round up so the final second is displayed as 1s rather than 0s. */
    long remaining_seconds = (remaining_ms + 999) / 1000;
    snprintf(out, size, "retrying in %lds (attempt %d/%d)...", remaining_seconds,
             render->retry.next_attempt, render->retry.max_attempts);
}

void render_update_retry_label(struct render_ctx *render)
{
    if (!render->retry.deadline_ms)
        return;
    char label[64];
    render_retry_label(render, monotonic_ms(), label, sizeof(label));
    footer_set_label(render->footer, "retry", label);
}
