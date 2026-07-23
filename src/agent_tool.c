/* SPDX-License-Identifier: MIT */
#include "agent_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "util.h"
#include "render/ctrl_strip.h"

void agent_tool_call_init(struct agent_tool_call *tc, const struct item *call)
{
    memset(tc, 0, sizeof(*tc));
    tc->original = call;
    tc->effective = *call;
    tc->tool = call->tool_name ? find_tool(call->tool_name) : NULL;

    if (tc->tool && tc->tool->preprocess_args && call->tool_arguments_json)
        tc->rewritten_args = tc->tool->preprocess_args(call->tool_arguments_json);
    if (tc->rewritten_args)
        tc->effective.tool_arguments_json = tc->rewritten_args;
}

void agent_tool_call_destroy(struct agent_tool_call *tc)
{
    free(tc->rewritten_args);
    memset(tc, 0, sizeof(*tc));
}

char *agent_tool_call_run(const struct agent_tool_call *tc, struct tool_ctx *ctx)
{
    if (!tc->tool)
        return xasprintf("unknown tool: %s",
                         tc->original->tool_name ? tc->original->tool_name : "?");
    return tc->tool->run(tc->effective.tool_arguments_json, ctx);
}

struct item agent_tool_result_make(const struct item *call, const char *output,
                                   struct tool_ctx *ctx)
{
    struct item it = {
        .kind = ITEM_TOOL_RESULT,
        .call_id = xstrdup(call->call_id),
        .output = ctrl_strip_dup(output ? output : ""),
    };
    if (ctx) {
        it.images = ctx->images;
        it.n_images = ctx->n_images;
        ctx->images = NULL;
        ctx->n_images = 0;
    }
    return it;
}

void image_budget_enforce(const struct item *history, size_t n_history, struct item *result)
{
    if (result->n_images == 0)
        return;
    size_t incoming = 0;
    for (size_t k = 0; k < result->n_images; k++)
        incoming += result->images[k].data_b64 ? strlen(result->images[k].data_b64) : 0;

    int over_bytes = images_total_b64(history, n_history) + incoming > IMAGE_REQUEST_BUDGET_B64;
    int over_count =
        images_total_count(history, n_history) + result->n_images > IMAGE_REQUEST_MAX_COUNT;
    if (!over_bytes && !over_count)
        return;

    for (size_t k = 0; k < result->n_images; k++) {
        free(result->images[k].mime);
        free(result->images[k].data_b64);
    }
    free(result->images);
    result->images = NULL;
    result->n_images = 0;

    /* Name whichever limit tripped: count is the tighter one for many small
     * images, bytes for a few large ones. */
    char cap[48];
    if (over_count)
        snprintf(cap, sizeof(cap), "holds too many images (max %d)", IMAGE_REQUEST_MAX_COUNT);
    else
        snprintf(cap, sizeof(cap), "is at its image budget (~%zu MB)",
                 (size_t)IMAGE_REQUEST_BUDGET_B64 / (1024 * 1024));
    char *note = xasprintf("%s\n\n[image not attached: this conversation %s. Ask the user to "
                           "/compact (summarizes and frees it) or /new.]",
                           result->output ? result->output : "", cap);
    free(result->output);
    result->output = note;
}
