/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_DISPATCH_H
#define HAX_AGENT_DISPATCH_H

#include "provider.h"          /* struct item */
#include "render/render_ctx.h" /* struct render_ctx */

/* Interactive tool-call rendering + dispatch for the REPL. Each entry
 * point renders the call (header, preview or silent breadcrumb) into the
 * live render state and returns the ITEM_TOOL_RESULT that goes back to
 * the model. The returned item owns its strings; the caller appends it
 * into the session vector. The one-shot path (oneshot.c) runs tools
 * inline without any of this display machinery. */

/* Top-level dispatch: pick the silent (quiet exploration cluster) or
 * verbose (header + preview) path based on the tool's silent_preview
 * flag and, for bash, per-call classification of the command. */
struct item dispatch_tool_call(struct render_ctx *r, const struct item *call, int image_input);

/* Render a synthesized "[interrupted]" block in place of running a tool,
 * and produce the matching tool_result so the conversation stays
 * well-formed when Esc fires partway through a batch. */
struct item dispatch_tool_skipped(struct render_ctx *r, const struct item *call);

/* Refuse a tool call without running it (--raw advertised no tools, so a
 * tool_call is a model bug or misbehaving backend). Renders a refusal
 * header and feeds back an error result the model can recover from. */
struct item dispatch_tool_refused(struct render_ctx *r, const struct item *call);

/* Render a collapsed, dim one-line view of a tool call — "[name] arg",
 * no output preview, no spinner, no execution. Used by history replay
 * (resume), where the tool can't be re-run and its live preview isn't
 * reconstructable from stored items; the header alone is enough
 * orientation. Writes a single newline-terminated line into the current
 * cursor position — the caller owns block separation (replay groups
 * consecutive calls under RS_CLUSTER so they stack tight). */
void render_collapsed_tool_call(struct render_ctx *r, const struct item *call);

#endif /* HAX_AGENT_DISPATCH_H */
