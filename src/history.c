/* SPDX-License-Identifier: MIT */
#include "history.h"

#include <jansson.h>

#include <stdlib.h>
#include <string.h>

#include "agent_core.h" /* INTERRUPT_MARKER */
#include "agent_dispatch.h"
#include "provider.h"
#include "tool.h"
#include "util.h"
#include "render/markdown.h"
#include "render/tool_render.h"
#include "terminal/ansi.h"
#include "terminal/input.h"

/* Echo a stored user message exactly as the live editor repaints a
 * submitted one — the accent "▌ " stripe + accent wrapped body — so a
 * rendered prompt is indistinguishable from one just typed. The editor
 * writes straight at the sink (bypassing disp), ending at column 0 of a
 * fresh row, so we resync disp afterward the same way agent_run does
 * after input_readline. */
static void render_user_echo(struct render_ctx *r, const char *text)
{
    render_open_block(r); /* one blank line above, cursor at column 0 */
    /* input_display_cols(), not term_width(): match the editor's configured
     * display width clamped to the tty, so a rendered prompt wraps identically
     * to a freshly typed one. */
    input_render_user_message_to(disp_sink(&r->disp), text ? text : "", text ? strlen(text) : 0,
                                 input_display_cols());
    r->disp.trail = 1;
    r->disp.held = 0;
}

/* Feed a stored assistant message or reasoning blob into the markdown
 * stream exactly as the live path would, so wrapping and block spacing
 * come out identical. NULL/empty is a no-op — opaque (Codex) reasoning has
 * no reasoning_text and renders as nothing, matching the live display. */
static void render_stored_text(struct render_ctx *r, enum render_state target, const char *text)
{
    if (!text || !*text)
        return;
    if (r->state != target) {
        /* Close the previous block to RS_IDLE first: render_transition's
         * close-half runs md_flush, emitting any deferred markdown tail
         * (an unmatched *, a backtick, a pending newline) to the terminal.
         * Only then reset md for the fresh block — resetting before the
         * flush would discard those bytes and silently drop characters.
         * This matches the live order, where md_reset runs while idle
         * between streams. saw_text=0 arms the first-text newline strip
         * for this block, like a fresh stream live. Skipped when already
         * in `target`: consecutive same-kind items concatenate into one
         * md stream (no reset, no re-strip mid-stream). */
        render_transition(r, RS_IDLE);
        if (r->md)
            md_reset(r->md, md_cols());
        r->disp.saw_text = 0;
    }
    if (target == RS_TEXT) {
        /* Same strip + open + feed the live text-delta path uses. */
        render_text_delta(r, text, strlen(text));
    } else {
        /* Reasoning: no leading-newline strip — the live reasoning-delta
         * path doesn't strip either. */
        render_transition(r, target);
        render_text_chunk(r, text, strlen(text));
    }
}

/* Render the standalone "[interrupted]" marker as its own dim out-of-band
 * block, rather than as plain assistant text. Live interrupts render no
 * marker line — there the resume hint above the next prompt is the
 * visible cue. */
static void render_interrupt_marker(struct render_ctx *r)
{
    render_open_block(r);
    disp_raw(&r->disp, ANSI_DIM);
    disp_printf(&r->disp, "%s", INTERRUPT_MARKER);
    disp_raw(&r->disp, ANSI_RESET);
    disp_putc(&r->disp, '\n');
    disp_flush(&r->disp);
}

/* Render a stored assistant message. The interrupt marker the agent appends
 * (ITEM_ORIGIN_INTERRUPTED) is split off and rendered through the dim
 * out-of-band block, matching live: a standalone "[interrupted]" (aborted
 * before output) shows only the dim block, and a partial response with
 * "\n[interrupted]" appended (aborted mid-text) shows the partial as normal
 * markdown then the dim block.
 *
 * Only the stamp makes it a marker — a model is free to end a real answer on
 * that exact line, and dimming *its* words into an out-of-band notice would
 * misreport the response. The suffix check that follows is arithmetic for
 * where the appended bytes start, not a test of whether they are ours. */
static void render_assistant(struct render_ctx *r, const struct item *it)
{
    const char *text = it->text;
    if (!text || !*text)
        return;
    size_t len = strlen(text);
    size_t mlen = strlen(INTERRUPT_MARKER);
    if (it->origin == ITEM_ORIGIN_INTERRUPTED && len >= mlen &&
        strcmp(text + len - mlen, INTERRUPT_MARKER) == 0) {
        size_t before = len - mlen; /* bytes before the marker */
        if (before == 0 || text[before - 1] == '\n') {
            if (before > 1) { /* a partial response precedes "\n[interrupted]" */
                char *partial = xasprintf("%.*s", (int)(before - 1), text);
                render_stored_text(r, RS_TEXT, partial);
                free(partial);
            }
            render_interrupt_marker(r);
            return;
        }
    }
    render_stored_text(r, RS_TEXT, text);
}

/* The bytes whose preview the user actually saw, when the result is a
 * stand-in for them: ITEM_ORIGIN_SUMMARIZED says the tool showed one thing and
 * returned another (`write` creating a file streams the content and returns
 * "created ..."), and unlike bash's raw stream those bytes are still here, in
 * the call's arguments. Which argument holds them is per-tool knowledge,
 * sniffed by name the way transcript.c and agent_dispatch.c pick display
 * treatment — but *whether* to go looking is the stored provenance, never the
 * shape of the output: every summary form is text a failing write could
 * produce ("created blocked exists but is not a regular file"), and replaying
 * the content over an error would show a write that never happened.
 *
 * Returns malloc'd with its length in *out_len (caller frees), or NULL to
 * use `output` as-is. The length comes from the JSON string rather than
 * strlen so an embedded NUL survives to the renderer's ctrl_strip, exactly
 * as it did live. */
static char *streamed_preview_body(const struct item *call, const struct item *result,
                                   size_t *out_len)
{
    *out_len = 0;
    if (result->origin != ITEM_ORIGIN_SUMMARIZED)
        return NULL;
    if (!call->tool_name || strcmp(call->tool_name, "write") != 0 || !call->tool_arguments_json)
        return NULL;
    json_t *root = json_loads(call->tool_arguments_json, 0, NULL);
    if (!root)
        return NULL;
    json_t *val = json_object_get(root, "content");
    char *body = NULL;
    if (json_is_string(val)) {
        size_t n = json_string_length(val);
        body = xmalloc(n + 1);
        memcpy(body, json_string_value(val), n);
        body[n] = '\0';
        *out_len = n;
    }
    json_decref(root);
    return body;
}

/* Rebuild a verbose call's output preview through the same renderer the
 * live preview ran. Feeding a complete string in one shot is not a special
 * case: it is exactly what dispatch does for every non-streaming tool
 * (write/edit always, bash when it returns without emitting), so head caps,
 * tail rings and elision markers all come out as they did live.
 *
 * Not byte-identical for a streaming tool: bash's live preview is driven
 * by the raw stream, while history holds the canonical return — already
 * ctrl_stripped and head/tail-truncated by bash itself, with its own
 * "[output truncated: ...]" marker in the text. So for bash the preview is
 * a preview of the *stored* output, which is all a later view of the
 * conversation has. */
static void render_result_preview(struct render_ctx *r, const struct item *call,
                                  const struct item *result)
{
    const char *output = result->output;
    const struct tool *t = call->tool_name ? find_tool(call->tool_name) : NULL;
    size_t streamed_len = 0;
    char *streamed = streamed_preview_body(call, result, &streamed_len);
    const char *body = streamed ? streamed : output;
    size_t body_len = streamed ? streamed_len : (output ? strlen(output) : 0);
    if (!body || body_len == 0) {
        if (streamed)
            /* A created-but-empty file: nothing was displayed live either,
             * so the summary stands in (`output` is non-empty whenever a
             * streamed body exists). */
            render_tool_solo_marker(r, output);
        else if (t && t->output_is_diff)
            /* Empty output from a diff-capable tool is the no-op
             * write/edit, marked inline live rather than as an empty block. */
            render_tool_solo_marker(r, "(no changes)");
        free(streamed);
        return;
    }
    enum render_mode mode = (t && t->preview_tail) ? R_HEAD_TAIL : R_HEAD_ONLY;
    struct tool_render rr;
    /* No spinner: every spinner entry point is NULL-safe, so the status
     * row simply never paints and each line commits as a permanent row. */
    tool_render_init(&rr, &r->disp, NULL, mode);
    if (t && t->output_is_diff && !streamed && strncmp(body, "--- ", 4) == 0)
        rr.mode = R_DIFF;
    tool_render_feed(&rr, body, body_len);
    tool_render_finalize(&rr);
    /* Content that renders no rows (blank, whitespace, control-only) would
     * leave a bare header, so fall back to the summary — the same decision
     * dispatch makes live, on the same actual row count. */
    if (streamed && rr.rows_emitted == 0 && output && *output)
        render_tool_solo_marker(r, output);
    tool_render_free(&rr);
    free(streamed);
}

/* The result paired with the call at items[i], by call_id. Returns NULL
 * for an orphan call (a batch cut short by an interrupt). A parallel batch
 * is stored (C1, C2, R1, R2) — the wire order — so the result is not
 * necessarily adjacent, but it is always in the same turn: the loop appends
 * every result before the next turn's boundary (agent_loop.c). The search
 * stops at `turn_to` because a call_id only has to be unique within its
 * response — local OpenAI-compatible backends happily reuse `call_0` — so
 * scanning on would let an orphan adopt an unrelated later result. */
static const struct item *paired_result(const struct item *items, size_t turn_to, size_t i)
{
    const char *id = items[i].call_id;
    if (!id)
        return NULL;
    for (size_t j = i + 1; j < turn_to; j++) {
        if (items[j].kind == ITEM_TOOL_RESULT && items[j].call_id &&
            strcmp(items[j].call_id, id) == 0)
            return &items[j];
    }
    return NULL;
}

/* The marker live showed in place of a block for a call the dispatcher
 * answered without ever running it: an Esc that cut a batch short
 * (dispatch_tool_skipped) or a --raw refusal (dispatch_tool_refused). Both
 * drew a verbose header plus the marker whatever the tool's usual silence,
 * so the outcome has to survive replay — collapsed to a quiet `[read]`
 * breadcrumb it would read as a call that ran and returned nothing.
 *
 * Read off item.origin, not the result text: the stored output is deliberately
 * something the model can act on, and a tool that ran can print those same
 * bytes (`printf '[interrupted]'`) — replaying that as a refusal would be the
 * view inventing an outcome. Returns NULL for a call that ran. */
static const char *undispatched_marker(const struct item *result)
{
    if (!result)
        return NULL;
    if (result->origin == ITEM_ORIGIN_SKIPPED)
        return INTERRUPT_MARKER;
    if (result->origin == ITEM_ORIGIN_REFUSED)
        return REFUSED_MARKER;
    return NULL;
}

static void render_tool_call(struct render_ctx *r, enum history_detail detail,
                             const struct item *items, size_t turn_to, size_t i)
{
    /* Live dispatch renders the *preprocessed* args (agent_tool_call_init's
     * `effective`), while history keeps the model's original emission by
     * design (tool.h): `bash` drops a redundant `cd <cwd> &&` prefix and the
     * path tools relativize against cwd. Re-derive them so a replayed line
     * spells its argument the way the screen did. Both hooks are pure rewrites
     * over the args plus cwd/HOME, and NULL means "use the original", exactly
     * as in dispatch — including for the silence check below, which live also
     * runs on the effective copy (the current hooks can't change its answer:
     * bash_classify already treats a `cd` prefix as neutral, and a path
     * rewrite says nothing about `read`'s static flag). */
    const struct tool *t = items[i].tool_name ? find_tool(items[i].tool_name) : NULL;
    char *effective_args =
        t && t->preprocess_args ? t->preprocess_args(items[i].tool_arguments_json) : NULL;
    struct item call = items[i];
    if (effective_args)
        call.tool_arguments_json = effective_args;

    const struct item *result = paired_result(items, turn_to, i);
    const char *marker = undispatched_marker(result);
    if (marker) {
        render_transition(r, RS_IDLE);
        render_tool_call_header(r, &call);
        render_tool_solo_marker(r, marker);
    } else if (detail == HISTORY_BRIEF || tool_call_is_silent(&call)) {
        /* RS_CLUSTER groups consecutive calls under one block separator
         * and lets them stack tight (the next non-tool item transitions
         * out cleanly), the way quiet exploration reads live. */
        render_transition(r, RS_CLUSTER);
        render_collapsed_tool_call(r, &call);
    } else {
        render_transition(r, RS_IDLE);
        render_tool_call_header(r, &call);
        if (result)
            render_result_preview(r, &call, result);
    }
    free(effective_args);
}

/* Items the provider streamed, shown as they arrived — as opposed to the
 * tool calls, which the agent dispatches only after the stream ends. */
static int is_streamed_kind(enum item_kind kind)
{
    return kind == ITEM_USER_MESSAGE || kind == ITEM_ASSISTANT_MESSAGE || kind == ITEM_REASONING;
}

/* Render the streamed items in [from, to) in stored order, which for them is
 * screen order. */
static void render_streamed_range(struct render_ctx *r, const struct item *items, size_t from,
                                  size_t to)
{
    size_t prev_streamed = 0;
    int seen_streamed = 0;
    for (size_t i = from; i < to; i++) {
        const struct item *it = &items[i];
        if (!is_streamed_kind(it->kind))
            continue;
        /* Two streamed items with something between them in stored order had
         * a tool call between them in the stream, and that closed the open
         * block live (render_stream_seam). Phase ordering moves the call out
         * of the way, so the positional gap is what's left to break on —
         * otherwise the text either side of a call runs together into one
         * paragraph. */
        if (seen_streamed && i != prev_streamed + 1)
            render_transition(r, RS_IDLE);
        prev_streamed = i;
        seen_streamed = 1;
        switch (it->kind) {
        case ITEM_USER_MESSAGE:
            /* A compaction seed is synthetic — mark the boundary the way
             * the live path's notice did instead of echoing the whole
             * summary as a typed prompt. */
            if (it->origin == ITEM_ORIGIN_COMPACT_SEED) {
                render_open_block(r);
                disp_raw(&r->disp, ANSI_DIM);
                disp_printf(&r->disp, "── conversation compacted ──");
                disp_raw(&r->disp, ANSI_RESET);
                disp_putc(&r->disp, '\n');
            } else if (it->origin == ITEM_ORIGIN_NONE) {
                render_user_echo(r, it->text);
            }
            /* Else: the continuation an empty send stands for. The prompt
             * row it would draw was never on screen — the user pressed
             * enter on an empty line and the answer followed. */
            break;
        case ITEM_ASSISTANT_MESSAGE:
            render_assistant(r, it);
            break;
        case ITEM_REASONING:
            /* A reasoning item closes the open block live whatever comes of
             * it — EV_REASONING_ITEM runs render_stream_seam before any
             * delta arrives (agent.c). So break here too when nothing will
             * be drawn (reasoning hidden, or opaque reasoning that carries
             * only provider JSON): without it an answer either side of the
             * reasoning runs together into one paragraph the model never
             * wrote. RS_CLUSTER is exempt, as it is in the seam. */
            if (r->show_reasoning && it->reasoning_text && *it->reasoning_text)
                render_stored_text(r, RS_REASONING, it->reasoning_text);
            else if (r->state != RS_CLUSTER)
                render_transition(r, RS_IDLE);
            break;
        case ITEM_TOOL_CALL:
        case ITEM_TOOL_RESULT:
        case ITEM_TURN_BOUNDARY:
        case ITEM_TURN_USAGE:
            break;
        }
    }
}

/* Render items[from .. to) — one turn — in the phases the live path had.
 * Everything the provider streamed is displayed while the stream runs; the
 * tools it asked for are dispatched only once the turn is complete, so text
 * that arrived *after* a tool call still appeared above that call's block.
 * Replaying items positionally would hoist the block above text that preceded
 * it on screen, so streamed content goes first here and the calls follow.
 *
 * The exception is what the agent appends once dispatch is done: results, and
 * behind them a synthetic "[interrupted]" assistant item when Esc landed after
 * the tools ran (agent_session_mark_interrupt). Nothing streamed can sit after
 * a result, so the last result is the seam — items past it were never part of
 * the stream and belong below the blocks, where the screen had them. */
static void render_turn(struct render_ctx *r, enum history_detail detail, const struct item *items,
                        size_t from, size_t to)
{
    size_t after_dispatch = to;
    for (size_t i = to; i > from; i--) {
        if (items[i - 1].kind == ITEM_TOOL_RESULT) {
            after_dispatch = i;
            break;
        }
    }
    render_streamed_range(r, items, from, after_dispatch);
    for (size_t i = from; i < to; i++) {
        if (items[i].kind == ITEM_TOOL_CALL)
            render_tool_call(r, detail, items, to, i);
        /* Results are rendered with their call (or deliberately dropped in
         * brief mode); an orphan result has no header to sit under. */
    }
    render_streamed_range(r, items, after_dispatch, to);
}

void history_render(struct render_ctx *r, enum history_detail detail, const struct item *items,
                    size_t n_items, size_t start_idx)
{
    size_t turn_start = start_idx;
    for (size_t i = start_idx; i < n_items; i++) {
        if (items[i].kind != ITEM_TURN_BOUNDARY)
            continue;
        render_turn(r, detail, items, turn_start, i);
        /* Each turn is its own stream live, and a new stream resets the
         * markdown renderer (repl_loop_turn_begin), so close an open text or
         * reasoning block here too: without the break, a turn ending in text
         * and the next one opening with text would run together into one
         * paragraph — a sentence neither of them said. RS_CLUSTER is exempt
         * because it is exempt live: a quiet exploration run spans turns
         * (see render_ctx.h), and breaking it would space out breadcrumb
         * lines the screen stacked tight. */
        if (r->state != RS_CLUSTER)
            render_transition(r, RS_IDLE);
        turn_start = i + 1;
    }
    render_turn(r, detail, items, turn_start, n_items);
}
