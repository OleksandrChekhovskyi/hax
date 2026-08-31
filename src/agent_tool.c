/* SPDX-License-Identifier: MIT */
#include "agent_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "provider.h"
#include "tool.h"
#include "xalloc.h"
#include "render/ctrl_strip.h"

void agent_tool_call_init(struct agent_tool_call *tc, const struct item *call)
{
    memset(tc, 0, sizeof(*tc));
    tc->original = call;
    tc->effective = *call;
    tc->tool = call->tool_name ? agent_find_tool(call->tool_name) : NULL;

    if (tc->tool && tc->tool->preprocess_args && call->tool_arguments_json)
        tc->owned_args_json = tc->tool->preprocess_args(call->tool_arguments_json);
    if (tc->owned_args_json)
        tc->effective.tool_arguments_json = tc->owned_args_json;
}

void agent_tool_call_destroy(struct agent_tool_call *tc)
{
    free(tc->owned_args_json);
    memset(tc, 0, sizeof(*tc));
}

char *agent_tool_call_run(const struct agent_tool_call *tc, struct tool_run_ctx *ctx)
{
    if (!tc->tool)
        return xasprintf("unknown tool: %s",
                         tc->original->tool_name ? tc->original->tool_name : "?");
    return tc->tool->run(tc->effective.tool_arguments_json, ctx);
}

struct item agent_tool_result_make(const struct item *call, const char *output,
                                   struct tool_run_ctx *ctx)
{
    struct item result = {
        .kind = ITEM_TOOL_RESULT,
        .call_id = xstrdup(call->call_id),
        .output = ctrl_strip_dup(output ? output : ""),
    };
    if (ctx) {
        result.images = ctx->result_images;
        result.n_images = ctx->n_result_images;
        ctx->result_images = NULL;
        ctx->n_result_images = 0;
        if (ctx->output_summarizes_display)
            result.origin = ITEM_ORIGIN_SUMMARIZED;
        /* A kill outranks display bookkeeping: resume decisions hinge on this provenance. */
        if (ctx->interrupted)
            result.origin = ITEM_ORIGIN_INTERRUPTED;
        result.output_hidden_tail = ctx->output_hidden_tail;
    }
    return result;
}

void agent_tool_result_enforce_image_budget(const struct item *history, size_t n_history,
                                            struct item *result)
{
    if (result->n_images == 0)
        return;
    size_t incoming_bytes = 0;
    for (size_t i = 0; i < result->n_images; i++)
        incoming_bytes += result->images[i].data_b64 ? strlen(result->images[i].data_b64) : 0;

    int exceeds_bytes = items_image_base64_bytes(history, n_history) + incoming_bytes >
                        IMAGE_REQUEST_BASE64_BUDGET_BYTES;
    int exceeds_count =
        items_image_count(history, n_history) + result->n_images > IMAGE_REQUEST_MAX_COUNT;
    if (!exceeds_bytes && !exceeds_count)
        return;

    for (size_t i = 0; i < result->n_images; i++) {
        free(result->images[i].mime);
        free(result->images[i].data_b64);
    }
    free(result->images);
    result->images = NULL;
    result->n_images = 0;

    char limit_description[48];
    if (exceeds_count)
        snprintf(limit_description, sizeof(limit_description), "holds too many images (max %d)",
                 IMAGE_REQUEST_MAX_COUNT);
    else
        snprintf(limit_description, sizeof(limit_description), "is at its image budget (~%zu MB)",
                 (size_t)IMAGE_REQUEST_BASE64_BUDGET_BYTES / (1024 * 1024));
    size_t body_len = result->output ? strlen(result->output) : 0;
    char *note = xasprintf("%s\n\n[image not attached: this conversation %s. Ask the user to "
                           "/compact (summarizes and frees it) or /new.]",
                           result->output ? result->output : "", limit_description);
    free(result->output);
    result->output = note;
    /* Appended after the tool displayed its bytes, so the note is model-only; it extends any
     * hidden tail already ending the output. */
    result->output_hidden_tail += strlen(note) - body_len;
}
