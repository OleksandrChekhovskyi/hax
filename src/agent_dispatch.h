/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_DISPATCH_H
#define HAX_AGENT_DISPATCH_H

#include "provider.h"          /* struct item */
#include "render/render_ctx.h" /* struct render_ctx */

/* REPL tool rendering/dispatch. Returned ITEM_TOOL_RESULT values own their
 * strings; one-shot runs tools without this display layer. */

/* Top-level dispatch: pick the silent (quiet exploration cluster) or
 * verbose (header + preview) path based on the tool's silent_preview
 * flag and, for bash, per-call classification of the command. */
struct item dispatch_tool_call(struct render_ctx *r, const struct item *call);

/* Render a synthesized "[interrupted]" block in place of running a tool,
 * and produce the matching tool_result so the conversation stays
 * well-formed when Esc fires partway through a batch. */
struct item dispatch_tool_skipped(struct render_ctx *r, const struct item *call);

/* Refuse a tool call without running it (--raw advertised no tools, so a
 * tool_call is a model bug or misbehaving backend). Renders a refusal
 * header and feeds back an error result the model can recover from. */
struct item dispatch_tool_refused(struct render_ctx *r, const struct item *call);

/* Render a dim one-line historical tool call. Resume cannot rerun the tool or
 * reconstruct its live preview, so only the header/arg is replayed. */
void render_collapsed_tool_call(struct render_ctx *r, const struct item *call);

#endif /* HAX_AGENT_DISPATCH_H */
