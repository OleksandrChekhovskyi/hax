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
 * header and feeds back an error result the model can recover from
 * (REFUSED_RESULT / REFUSED_MARKER in agent_core.h). */
struct item dispatch_tool_refused(struct render_ctx *r, const struct item *call);

/* Render a collapsed, dim one-line view of a tool call — "[name] arg",
 * no output preview, no spinner, no execution. Used by the resume replay,
 * where one screenful is the whole budget, and for silent (quiet cluster)
 * calls in the history view, which showed no output live either. Writes a
 * single newline-terminated line into the current cursor position — the
 * caller owns block separation (replay groups consecutive calls under
 * RS_CLUSTER so they stack tight). */
void render_collapsed_tool_call(struct render_ctx *r, const struct item *call);

/* The verbose call header — block separator, `[name]` tag, reflowed
 * display_arg — that opens a tool block live. Exposed for the history
 * view, which re-renders stored calls in the same idiom. */
void render_tool_call_header(struct render_ctx *r, const struct item *call);

/* One dim "›" row carrying a terse tool outcome that bypasses the output
 * preview ("(no changes)", write's created-file summary). `text` may be
 * model-controlled, so it is flattened to one row and truncated. */
void render_tool_solo_marker(struct render_ctx *r, const char *text);

/* Whether `call` renders through the quiet cluster path — no header, no
 * output preview, just a breadcrumb line. Static `silent_preview` flag,
 * or the tool's per-call `is_silent` classifier (bash inspects the
 * command). The history view asks so a replayed call is never louder
 * than it was live. */
int tool_call_is_silent(const struct item *call);

#endif /* HAX_AGENT_DISPATCH_H */
