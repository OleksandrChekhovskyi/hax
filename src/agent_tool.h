/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_TOOL_H
#define HAX_AGENT_TOOL_H

#include "provider.h"
#include "tool.h"

/* Prepared local view of a model-issued call. original and tool are borrowed; effective is a
 * shallow copy that may point to owned_args_json. */
struct agent_tool_call {
    const struct item *original;
    struct item effective;
    const struct tool *tool;
    char *owned_args_json;
};

/* Resolve the tool and preprocess its arguments without modifying conversation history. */
void agent_tool_call_init(struct agent_tool_call *tc, const struct item *call);
void agent_tool_call_destroy(struct agent_tool_call *tc);

/* Returns the tool's allocated output, or an allocated unknown-tool error. */
char *agent_tool_call_run(const struct agent_tool_call *tc, struct tool_run_ctx *ctx);

/* Build an owned result, sanitize output, and move result images out of ctx when non-NULL. */
struct item agent_tool_result_make(const struct item *call, const char *output,
                                   struct tool_run_ctx *ctx);

/* If result exceeds the aggregate image limits, drop its images and append a recoverable note.
 * Existing history remains byte-stable for provider prefix caching. */
void agent_tool_result_enforce_image_budget(const struct item *history, size_t n_history,
                                            struct item *result);

#endif /* HAX_AGENT_TOOL_H */
