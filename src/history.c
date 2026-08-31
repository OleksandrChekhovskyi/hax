/* SPDX-License-Identifier: MIT */
#include "history.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "agent_dispatch.h"
#include "agent_tool.h"
#include "provider.h"
#include "tool.h"
#include "xalloc.h"
#include "render/disp.h"
#include "render/markdown.h"
#include "render/render_ctx.h"
#include "render/tool_render.h"
#include "terminal/ansi.h"
#include "terminal/input.h"

static void render_user_message(struct render_ctx *render, const char *text)
{
    render_open_block(render);
    /* Match the editor's configured width rather than the raw terminal width. */
    input_render_user_message_to(disp_sink(&render->disp), text ? text : "",
                                 text ? strlen(text) : 0, input_display_cols());
    /* The input renderer bypasses disp and finishes at column zero. */
    disp_sync_external_line(&render->disp);
}

static void render_stored_text(struct render_ctx *render, enum render_mode mode, const char *text)
{
    if (!text || !*text)
        return;
    if (render->mode != mode) {
        /* Flush deferred markdown before resetting its state for a new block. */
        render_set_mode(render, RENDER_IDLE);
        if (render->md)
            md_reset(render->md, md_cols());
        render->stream.answer_started = 0;
    }
    if (mode == RENDER_TEXT) {
        render_text_delta(render, text, strlen(text));
    } else {
        render_set_mode(render, mode);
        render_write_text(render, text, strlen(text));
    }
}

/* Visible marker text must reset committed_newlines before the next block separator. */
static void render_dim_marker(struct render_ctx *render, const char *text)
{
    render_open_block(render);
    disp_write_ansi(&render->disp, ANSI_DIM);
    disp_write(&render->disp, text, strlen(text));
    disp_write_ansi(&render->disp, ANSI_RESET);
    disp_putc(&render->disp, '\n');
    disp_flush(&render->disp);
}

static void render_interrupt_marker(struct render_ctx *render)
{
    render_dim_marker(render, INTERRUPT_MARKER);
}

/* Provenance, not marker text, distinguishes an interrupt from model-authored output. */
static void render_assistant(struct render_ctx *render, const struct item *item)
{
    const char *text = item->text;
    if (!text || !*text)
        return;
    size_t text_len = strlen(text);
    size_t marker_len = strlen(INTERRUPT_MARKER);
    if (item->origin == ITEM_ORIGIN_INTERRUPTED && text_len >= marker_len &&
        strcmp(text + text_len - marker_len, INTERRUPT_MARKER) == 0) {
        size_t marker_offset = text_len - marker_len;
        if (marker_offset == 0 || text[marker_offset - 1] == '\n') {
            if (marker_offset > 1) {
                size_t partial_len = marker_offset - 1;
                char *partial = xmalloc(partial_len + 1);
                memcpy(partial, text, partial_len);
                partial[partial_len] = '\0';
                render_stored_text(render, RENDER_TEXT, partial);
                free(partial);
            }
            render_interrupt_marker(render);
            return;
        }
    }
    render_stored_text(render, RENDER_TEXT, text);
}

/* Trust summarized provenance rather than output text, which tools may reproduce on failure.
 * JSON length preserves embedded NUL bytes from the original display. */
static char *summarized_preview_body(const struct item *call, const struct item *result,
                                     size_t *body_len)
{
    *body_len = 0;
    if (result->origin != ITEM_ORIGIN_SUMMARIZED)
        return NULL;
    if (!call->tool_name || strcmp(call->tool_name, "write") != 0 || !call->tool_arguments_json)
        return NULL;

    json_t *root = json_loads(call->tool_arguments_json, 0, NULL);
    if (!root)
        return NULL;
    json_t *content = json_object_get(root, "content");
    char *body = NULL;
    if (json_is_string(content)) {
        *body_len = json_string_length(content);
        body = xmalloc(*body_len + 1);
        memcpy(body, json_string_value(content), *body_len);
        body[*body_len] = '\0';
    }
    json_decref(root);
    return body;
}

/* Streaming tools can only replay their stored canonical output, not the original byte stream. */
static void render_result_preview(struct render_ctx *render, const struct item *call,
                                  const struct item *result, enum tool_preview_mode preview_mode)
{
    const char *output = result->output;
    const struct tool *tool = call->tool_name ? agent_find_tool(call->tool_name) : NULL;
    int is_diff_output = tool && tool->display.output_style == TOOL_OUTPUT_UNIFIED_DIFF;
    size_t reconstructed_len = 0;
    char *reconstructed = summarized_preview_body(call, result, &reconstructed_len);
    const char *body = reconstructed ? reconstructed : output;
    size_t body_len = reconstructed ? reconstructed_len : (output ? strlen(output) : 0);
    /* A hidden tail was never displayed live, so the replay drops it too. */
    if (!reconstructed && result->output_hidden_tail <= body_len)
        body_len -= result->output_hidden_tail;
    if (!body || body_len == 0) {
        if (reconstructed)
            render_tool_solo_marker(render, output);
        else if (is_diff_output)
            render_tool_solo_marker(render, "(no changes)");
        free(reconstructed);
        return;
    }

    enum tool_render_mode mode =
        preview_mode == TOOL_PREVIEW_HEAD_TAIL ? TOOL_RENDER_HEAD_TAIL : TOOL_RENDER_HEAD;
    struct tool_render renderer;
    tool_render_init(&renderer, &render->disp, NULL, mode);
    if (is_diff_output && !reconstructed && strncmp(body, "--- ", 4) == 0)
        tool_render_set_mode(&renderer, TOOL_RENDER_DIFF);
    tool_render_feed(&renderer, body, body_len);
    tool_render_finalize(&renderer);
    /* Control stripping can leave reconstructed display bytes with no visible rows. */
    if (reconstructed && renderer.rows_emitted == 0 && output && *output)
        render_tool_solo_marker(render, output);
    tool_render_free(&renderer);
    free(reconstructed);
}

/* Search only this turn because compatible backends may reuse call IDs across responses. */
static const struct item *paired_result(const struct item *items, size_t turn_end, size_t call_idx)
{
    const char *call_id = items[call_idx].call_id;
    if (!call_id)
        return NULL;
    for (size_t result_idx = call_idx + 1; result_idx < turn_end; result_idx++) {
        if (items[result_idx].kind == ITEM_TOOL_RESULT && items[result_idx].call_id &&
            strcmp(items[result_idx].call_id, call_id) == 0)
            return &items[result_idx];
    }
    return NULL;
}

/* Provenance prevents ordinary tool output matching a marker from becoming undispatched. */
static const char *undispatched_marker(const struct item *result)
{
    if (!result)
        return NULL;
    if (result->origin == ITEM_ORIGIN_SKIPPED)
        return INTERRUPT_MARKER;
    if (result->origin == ITEM_ORIGIN_REFUSED)
        return REFUSED_MARKER;
    return NULL;
}

static void render_tool_call(struct render_ctx *render, enum history_detail detail,
                             const struct item *items, size_t turn_end, size_t call_idx)
{
    struct agent_tool_call prepared;
    agent_tool_call_init(&prepared, &items[call_idx]);
    const struct item *call = &prepared.effective;

    const struct item *result = paired_result(items, turn_end, call_idx);
    const char *marker = undispatched_marker(result);
    enum tool_preview_mode preview_mode = tool_call_preview_mode(call);
    if (marker) {
        render_set_mode(render, RENDER_IDLE);
        render_tool_call_header(render, call);
        render_tool_solo_marker(render, marker);
    } else if (detail == HISTORY_BRIEF || preview_mode == TOOL_PREVIEW_COLLAPSED) {
        render_set_mode(render, RENDER_TOOL_CLUSTER);
        render_collapsed_tool_call(render, call);
    } else {
        render_set_mode(render, RENDER_IDLE);
        render_tool_call_header(render, call);
        if (result)
            render_result_preview(render, call, result, preview_mode);
    }
    agent_tool_call_destroy(&prepared);
}

static int is_streamed_kind(enum item_kind kind)
{
    return kind == ITEM_USER_MESSAGE || kind == ITEM_ASSISTANT_MESSAGE || kind == ITEM_REASONING;
}

static void render_compaction_marker(struct render_ctx *render)
{
    render_dim_marker(render, "── conversation compacted ──");
}

static void render_streamed_range(struct render_ctx *render, const struct item *items, size_t from,
                                  size_t to)
{
    size_t previous_idx = to;
    for (size_t item_idx = from; item_idx < to; item_idx++) {
        const struct item *item = &items[item_idx];
        if (!is_streamed_kind(item->kind))
            continue;
        /* A positional gap contains a tool call, which closed the live streamed block. */
        if (previous_idx != to && item_idx != previous_idx + 1)
            render_set_mode(render, RENDER_IDLE);
        previous_idx = item_idx;

        switch (item->kind) {
        case ITEM_USER_MESSAGE:
            if (item->origin == ITEM_ORIGIN_COMPACT_SEED)
                render_compaction_marker(render);
            else if (item->origin == ITEM_ORIGIN_TASK_NOTE)
                render_dim_marker(render, item->text ? item->text : "");
            else if (item->origin == ITEM_ORIGIN_NONE)
                render_user_message(render, item->text);
            break;
        case ITEM_ASSISTANT_MESSAGE:
            render_assistant(render, item);
            break;
        case ITEM_REASONING:
            /* Hidden and opaque reasoning still create a stream seam. */
            if (render->show_reasoning && item->reasoning_text && *item->reasoning_text) {
                /* Consecutive reasoning items render as separate blocks, as in the live view. */
                if (render->mode == RENDER_REASONING)
                    render_set_mode(render, RENDER_IDLE);
                render_stored_text(render, RENDER_REASONING, item->reasoning_text);
            } else if (render->mode != RENDER_TOOL_CLUSTER) {
                render_set_mode(render, RENDER_IDLE);
            }
            break;
        case ITEM_TOOL_CALL:
        case ITEM_TOOL_RESULT:
        case ITEM_TURN_BOUNDARY:
        case ITEM_TURN_USAGE:
            break;
        }
    }
}

/* Provider-streamed items precede dispatched tool blocks; synthetic post-result items follow. */
static void render_turn(struct render_ctx *render, enum history_detail detail,
                        const struct item *items, size_t turn_start, size_t turn_end)
{
    size_t post_dispatch_from = turn_end;
    for (size_t item_idx = turn_end; item_idx > turn_start; item_idx--) {
        if (items[item_idx - 1].kind == ITEM_TOOL_RESULT) {
            post_dispatch_from = item_idx;
            break;
        }
    }

    render_streamed_range(render, items, turn_start, post_dispatch_from);
    for (size_t call_idx = turn_start; call_idx < turn_end; call_idx++) {
        if (items[call_idx].kind == ITEM_TOOL_CALL)
            render_tool_call(render, detail, items, turn_end, call_idx);
    }
    render_streamed_range(render, items, post_dispatch_from, turn_end);
}

void history_render(struct render_ctx *render, enum history_detail detail, const struct item *items,
                    size_t n_items, size_t start_idx)
{
    size_t turn_start = start_idx;
    for (size_t item_idx = start_idx; item_idx < n_items; item_idx++) {
        if (items[item_idx].kind != ITEM_TURN_BOUNDARY)
            continue;
        render_turn(render, detail, items, turn_start, item_idx);
        /* Markdown streams reset between turns; collapsed clusters intentionally span them. */
        if (render->mode != RENDER_TOOL_CLUSTER)
            render_set_mode(render, RENDER_IDLE);
        turn_start = item_idx + 1;
    }
    render_turn(render, detail, items, turn_start, n_items);
}
