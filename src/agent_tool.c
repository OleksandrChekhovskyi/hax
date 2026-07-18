/* SPDX-License-Identifier: MIT */
#include "agent_tool.h"

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

char *agent_tool_call_run(const struct agent_tool_call *tc, tool_emit_display_fn emit, void *user)
{
    if (!tc->tool)
        return xasprintf("unknown tool: %s",
                         tc->original->tool_name ? tc->original->tool_name : "?");
    return tc->tool->run(tc->effective.tool_arguments_json, emit, user);
}

struct item agent_tool_result_make(const struct item *call, const char *output)
{
    return (struct item){
        .kind = ITEM_TOOL_RESULT,
        .call_id = xstrdup(call->call_id),
        .output = ctrl_strip_dup(output ? output : ""),
    };
}
