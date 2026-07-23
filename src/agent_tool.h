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

/* Run the prepared call with the given per-call context (may be NULL — see
 * tool.h). Returns the tool's malloc'd canonical output, or a malloc'd
 * unknown-tool result. */
char *agent_tool_call_run(const struct agent_tool_call *tc, struct tool_ctx *ctx);

/* Build the model-facing result for call, stripping terminal control bytes
 * from output at the single history boundary and moving any images the tool
 * attached out of `ctx` (NULL for synthesized results — refusals, skips,
 * interrupt markers). The returned item owns its fields. */
struct item agent_tool_result_make(const struct item *call, const char *output,
                                   struct tool_ctx *ctx);

/* Enforce the aggregate image budget at ingestion: if attaching `result`'s
 * images to a conversation already holding `history` (n_history items, the
 * session so far, excluding `result`) would exceed IMAGE_REQUEST_BUDGET_B64,
 * drop them and append a recoverable note to the output. The just-read image
 * loses, not older ones — so images the model is already reasoning about
 * stay put and the request prefix (hence the provider's cache) is never
 * rewritten. No-op when the result carries no images or they fit. */
void image_budget_enforce(const struct item *history, size_t n_history, struct item *result);

#endif /* HAX_AGENT_TOOL_H */
