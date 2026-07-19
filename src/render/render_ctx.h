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
 *                line and keeps a parked spinner below the cluster
 *                across gaps. Survives stream() boundaries; the other
 *                states are reset per-stream. */
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
     * just entered, no header painted yet". cluster_line_open: the
     * current quiet line still awaits its terminating \n (emitted by
     * the transition close-half). cluster_line_used is the line's
     * exact cell count — cells, not bytes, because it doubles as the
     * parked spinner's cursor-restore column. */
    const char *cluster_last_tool;
    int cluster_line_open;
    int cluster_line_used;

    /* Stall clock for the table-composing spinner: ms of the most
     * recent live delta, 0 when unarmed. Only consulted while md is
     * buffering a table — pauses in ordinary streaming text
     * deliberately surface nothing (the text is its own progress
     * indicator). */
    long last_text_at;

    /* Table-composing spinner is up ("composing..." on its own row
     * while md buffers a GFM table and emits nothing).
     * render_text_chunk hides it before each md_feed so a finalizing
     * grid never races the spinner row, and re-shows if still
     * buffering; render_transition clears it on any real state
     * change. */
    int table_composing;

    /* Retry countdown — the tick repaints the spinner label from
     * these so the seconds visibly shrink during the backoff sleep.
     * retry_deadline_at == 0 means no retry pending. */
    long retry_deadline_at;
    int retry_attempt;
    int retry_max;

    /* The current stream has produced content (text, reasoning, or a
     * tool call) — the gate for soft-interrupt stream cancellation: a
     * request still prefilling has produced nothing a pause could lose,
     * so the tick aborts it and the pause lands immediately instead of
     * after a full extra turn. Reset per provider turn. */
    int stream_content_seen;

    /* Whether reasoning deltas are rendered or only drive the spinner. */
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

/* Pre-stream housekeeping: arm fresh stall/retry bookkeeping for the
 * new provider call, then show the busy "working..." spinner — unless
 * we're continuing a cluster, whose parked spinner is the alive
 * indicator across the gap. */
void render_stream_begin(struct render_ctx *r);

/* Return to a clean idle line and open a fresh visual block — for
 * out-of-band rendering (error line, usage stats, interrupt marker)
 * that should land separated from prior content. */
void render_open_block(struct render_ctx *r);

/* Write a chunk of model-produced text into the currently-open content
 * stream (RS_TEXT or RS_REASONING). Routes through md when enabled,
 * falls through to disp otherwise; keeps the table-stall clock fresh
 * so the tick can surface the composing spinner if a table buffer
 * goes quiet. */
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
