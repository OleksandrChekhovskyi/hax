/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_DISPATCH_H
#define HAX_AGENT_DISPATCH_H

#include "tool.h"
#include "render/render_ctx.h"

/* Render and run a tool call. The returned ITEM_TOOL_RESULT owns its fields. */
struct item dispatch_tool_call(struct render_ctx *render, const struct item *call, int image_input);

/* Render a call that was skipped after interruption and return its synthetic result. */
struct item dispatch_tool_skipped(struct render_ctx *render, const struct item *call);

/* Render a call refused by the frontend and return its synthetic result. */
struct item dispatch_tool_refused(struct render_ctx *render, const struct item *call);

/* Render a call as a collapsed breadcrumb without executing it. */
void render_collapsed_tool_call(struct render_ctx *render, const struct item *call);

/* Render the header that starts a verbose tool block. */
void render_tool_call_header(struct render_ctx *render, const struct item *call);

/* Render a flattened, terminal-width-limited outcome row. */
void render_tool_solo_marker(struct render_ctx *render, const char *text);

/* Resolve the call's configured and per-call preview mode. */
enum tool_preview_mode tool_call_preview_mode(const struct item *call);

#endif /* HAX_AGENT_DISPATCH_H */
