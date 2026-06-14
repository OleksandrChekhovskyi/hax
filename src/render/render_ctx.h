/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_CTX_H
#define HAX_RENDER_CTX_H

#include <stddef.h>

#include "render/disp.h"

struct spinner;
struct md_renderer;

/* What visual mode is currently "open" on the terminal — each state
 * owns specific bytes (SGR runs, cursor position, in-flight spinner
 * variant) that must be unwound before drawing anything else.
 *
 *   RS_IDLE      Nothing open: cursor at column 0, no SGR active.
 *   RS_WAITING   The agent is busy but no content block is open; owns the
 *                full-size line spinner in its own block.
 *   RS_REASONING Dim+italic CoT span open. Only reached when
 *                HAX_SHOW_REASONING is on; otherwise reasoning drives
 *                the spinner label only.
 *   RS_TEXT      Markdown answer stream open; md owns inline SGR.
 *   RS_CLUSTER   In-progress run of "quiet" tool calls (read/grep
 *                exploration). Coalesces consecutive `read`s onto one
 *                line and keeps its own end-of-line spinner across
 *                gaps. Survives stream() boundaries; the other states
 *                are reset per-stream. */
enum render_state {
    RS_IDLE,
    RS_WAITING,
    RS_REASONING,
    RS_TEXT,
    RS_CLUSTER,
};

/* All rendering state. Threaded through call sites as a single
 * pointer; stack-allocated in agent_run with spinner/md as opaque
 * handles owned (and freed) there. */
struct render_ctx {
    enum render_state state;

    struct disp disp;
    struct spinner *spinner;
    struct md_renderer *md; /* NULL when markdown is disabled */

    /* RS_CLUSTER sub-state. cluster_last_tool == NULL means "cluster
     * just entered, no header painted yet". cluster_line_used is a
     * cell budget against terminal width for read-coalescing wrap. */
    const char *cluster_last_tool;
    int cluster_line_used;

    /* Idle inline overlay (SPINNER_INLINE_TEXT) — drawn on top of an
     * active text/reasoning stream when the model goes quiet.
     * last_text_at is the ms of the most recent live delta; 0 means
     * no idle window armed. idle_shown latches once drawn so the
     * tick doesn't re-emit it every second. */
    long last_text_at;
    int idle_shown;

    /* Table-composing overlay (SPINNER_LINE "composing...") — drawn on
     * its own row while md is buffering a GFM table, a window that emits
     * nothing until the whole grid lays out. Distinct from idle_shown:
     * the bytes are arriving (not a stall), so it's keyed on md_in_table
     * rather than the idle clock alone, and it persists across the silent
     * row-buffering deltas. render_text_chunk hides it before each md_feed
     * (so a finalizing grid never races the spinner row) and re-shows if
     * still buffering; render_transition clears it on any real state
     * change (so the end-of-stream md_flush renders the grid cleanly). */
    int table_composing;

    /* Deferred tool-call "composing..." label. EV_TOOL_CALL_START knows
     * the tool name but holds the busy spinner on "working..." and arms
     * these instead of swapping the label immediately; the stream tick
     * performs the swap only once the compose window outlives
     * TOOL_COMPOSE_LABEL_MS, so a fast call dispatches first and never
     * flickers working->composing->header. compose_at == 0 means no
     * pending swap (latched off after the swap, and cleared by any event
     * that sets its own label). compose_label holds the formatted text.
     * composing_active latches once the swap has happened this stream, so
     * a following call in a back-to-back batch swaps straight to its name
     * instead of reverting to "working..." and re-deferring (which would
     * flicker between calls). compose_end_at is the symmetric revert timer:
     * armed at EV_TOOL_CALL_END while composing, it lets the tick fall back
     * to "working..." only if the model stalls past TOOL_COMPOSE_LABEL_MS
     * after finalizing the args (a following call or the done event cancels
     * it first). All three reset per stream in render_stream_begin. */
    long compose_at;
    char compose_label[64];
    int composing_active;
    long compose_end_at;

    /* Retry countdown — the tick repaints the spinner label from
     * these so the seconds visibly shrink during the backoff sleep.
     * retry_deadline_at == 0 means no retry pending. */
    long retry_deadline_at;
    int retry_attempt;
    int retry_max;

    /* HAX_SHOW_REASONING — when 0, reasoning deltas drive only the
     * spinner label; no RS_REASONING transition is taken. */
    int show_reasoning;
};

/* Move the live render state to `to`: close whatever is currently
 * open (SGR reset, md tail flush, state-owned spinner hidden), then
 * open the target (block separator, fresh SGR, canonical label). */
void render_transition(struct render_ctx *r, enum render_state to);

/* A streamed item boundary means the prior visible content block is done,
 * but the provider is still active. Keep silent clusters intact so they can
 * coalesce with the tool call that will be classified during dispatch. */
void render_stream_seam(struct render_ctx *r);

/* Pre-stream housekeeping: arm fresh idle/retry bookkeeping for the
 * new provider call, then show the busy "working..." spinner — unless
 * we're continuing a cluster, whose own end-of-line spinner is the alive
 * indicator across the gap. */
void render_stream_begin(struct render_ctx *r);

/* Return to a clean idle line and open a fresh visual block — for
 * out-of-band rendering (error line, usage stats, interrupt marker)
 * that should land separated from prior content. */
void render_open_block(struct render_ctx *r);

/* Write a chunk of model-produced text into the currently-open content
 * stream (RS_TEXT or RS_REASONING). Routes through md when enabled,
 * falls through to disp otherwise; arms idle detection so the tick
 * can surface the inline spinner if the model goes quiet. */
void render_text_chunk(struct render_ctx *r, const char *s, size_t n);

/* Render a chunk of assistant answer text into RS_TEXT: peek-strip leading
 * newlines on the first chunk of the block (disp tracks "first" via
 * saw_text, which the caller clears per stream/item) so a provider's
 * trailing-NL convention doesn't open a stray blank line above the answer,
 * then transition to RS_TEXT and feed the rest. A chunk that is all leading
 * newlines collapses to nothing (no transition — the spinner stays up).
 * Shared by the live text-delta handler and resume replay so both strip
 * identically. */
void render_text_delta(struct render_ctx *r, const char *s, size_t n);

/* Show the labeled "composing..." line spinner for an in-progress table
 * buffer: flush any held newline so it lands on a fresh row below the
 * preceding block, then raise SPINNER_LINE. Idempotent via
 * r->table_composing. Called from the stream tick (slow table) and
 * re-issued by render_text_chunk across the silent row-buffering deltas. */
void render_table_spinner_show(struct render_ctx *r);

/* Paint the spinner with the retry countdown label. Called on EV_RETRY
 * (so the user sees the new state immediately) and from the stream tick
 * (so the seconds count visibly decreases during the backoff sleep). */
void update_retry_label(struct render_ctx *r);

#endif /* HAX_RENDER_CTX_H */
