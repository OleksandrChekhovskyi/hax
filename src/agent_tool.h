/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_TOOL_H
#define HAX_AGENT_TOOL_H

#include "provider.h"
#include "tool.h"

/* Prepared view of one model-issued tool call. The original item remains in
 * conversation history; effective is a shallow copy whose arguments may point
 * at the owned rewritten_args for local display and execution. */
struct agent_tool_call {
    const struct item *original;
    struct item effective;
    const struct tool *tool;
    char *rewritten_args;
};

/* Resolve the tool and apply its optional argument preprocessor once. */
void agent_tool_call_init(struct agent_tool_call *tc, const struct item *call);
void agent_tool_call_destroy(struct agent_tool_call *tc);

/* Run the prepared call, forwarding the optional display side channel. Returns
 * the tool's malloc'd canonical output, or a malloc'd unknown-tool result. */
char *agent_tool_call_run(const struct agent_tool_call *tc, tool_emit_display_fn emit, void *user);

/* Build the model-facing result for call, stripping terminal control bytes from
 * output at the single history boundary. The returned item owns its fields. */
struct item agent_tool_result_make(const struct item *call, const char *output);

#endif /* HAX_AGENT_TOOL_H */
