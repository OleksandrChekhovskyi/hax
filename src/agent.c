/* SPDX-License-Identifier: MIT */
#include "agent.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "agent_dispatch.h"
#include "config.h"
#include "session.h"
#include "slash.h"
#include "tool.h"
#include "transcript.h"
#include "turn.h"
#include "util.h"
#include "render/disp.h"
#include "render/markdown.h"
#include "render/render_ctx.h"
#include "render/spinner.h"
#include "system/keepawake.h"
#include "system/spawn.h"
#include "terminal/ansi.h"
#include "terminal/input.h"
#include "terminal/interrupt.h"
#include "terminal/notify.h"
#include "terminal/ui.h"

/* The ASCII fallback is used when locale_init_utf8() couldn't establish a
 * UTF-8 LC_CTYPE — wcwidth() under a non-UTF-8 locale would mis-account
 * the multibyte glyph and break cursor positioning. */
#define PROMPT_UTF8  ANSI_BRIGHT_MAGENTA ANSI_BOLD "❯" ANSI_BOLD_OFF ANSI_FG_DEFAULT " "
#define PROMPT_ASCII ANSI_BOLD ">" ANSI_BOLD_OFF " "

/* Outcome of absorb_aborted_turn — drives whether a caller adds a
 * fallback standalone [interrupted] marker.
 *
 *   had_state    Anything was streamed before the abort: in-flight
 *                text, an in-progress tool_call (start+args without
 *                a matching end), or completed items already pushed
 *                this turn (text flushed by EV_TOOL_CALL_START, a
 *                completed reasoning blob, etc.). 0 means the error
 *                fired before any wire content arrived (pre-stream
 *                500/401/429 after retries) — there's nothing for
 *                the model to "continue from".
 *   marker_placed [interrupted] landed inside the absorbed batch
 *                (in flushed text or as a synthesized tool_result). */
struct absorb_outcome {
    int had_state;
    int marker_placed;
};

/* Shape conversation history for an aborted assistant turn — used by
 * both the mid-stream EV_ERROR path (network drop, length truncation)
 * and the Esc-cancel path. Both leave partial state behind that needs
 * the same three-step cleanup so the next user turn lands in a
 * well-formed conversation:
 *
 *   1. Tag any in-flight assistant text with [interrupted] before
 *      flushing — without the marker, the model on the next turn
 *      would see a complete-looking response and not know to
 *      continue or recover.
 *   2. Absorb the turn's items (the tagged text, any completed
 *      tool_calls) into the session vector.
 *   3. For each completed tool_call in the absorbed batch, synthesize
 *      a matching [interrupted] tool_result — none of them ran (we
 *      break before dispatch), and a tool_call without a matching
 *      result is malformed history that future turns reject.
 *
 * Returns the outcome flags so callers can decide whether to add a
 * standalone [interrupted] assistant message:
 *
 *   - Esc path: always add the fallback when marker_placed=0 (user
 *     cancellation needs explicit ack regardless of whether anything
 *     streamed).
 *   - EV_ERROR path: only add when had_state=1 && marker_placed=0
 *     (tag the "text-flushed-by-tool_call_start, then error" gap;
 *     skip when nothing streamed — a pre-stream 500 with no output
 *     would otherwise leave a fake assistant turn in history that
 *     misleads the model on the user's next prompt). */
static struct absorb_outcome absorb_aborted_turn(struct agent_session *sess, struct turn *t)
{
    /* Capture state-before-mutation. n_items already covers any
     * earlier flush (e.g. EV_TOOL_CALL_START flushed text on its
     * way through); n_pending covers an in-flight tool_call that
     * never reached EV_TOOL_CALL_END. in_reasoning covers a
     * reasoning-only stream that errored/cancelled before any
     * text/tool event flushed the buffered CoT — preserve it (the
     * whole point of the reasoning round-trip) rather than dropping
     * it in turn_reset. Flush it before the text so the reasoning
     * item precedes the assistant message, matching the normal flow. */
    int had_state = t->in_text || t->in_reasoning || t->n_pending > 0 || t->n_items > 0;
    int had_partial_text = t->in_text;
    turn_flush_reasoning(t);
    turn_flush_text(t, had_partial_text ? "\n" INTERRUPT_MARKER : NULL);

    size_t n_before;
    int had_tc;
    agent_session_absorb(sess, t, &n_before, &had_tc);
    (void)had_tc;
    turn_reset(t);

    int marker_placed = had_partial_text;
    size_t end = sess->n_items;
    for (size_t i = n_before; i < end; i++) {
        if (sess->items[i].kind == ITEM_TOOL_CALL) {
            items_append(&sess->items, &sess->n_items, &sess->cap_items,
                         (struct item){
                             .kind = ITEM_TOOL_RESULT,
                             .call_id = xstrdup(sess->items[i].call_id),
                             .output = xstrdup(INTERRUPT_MARKER),
                         });
            marker_placed = 1;
        }
    }
    return (struct absorb_outcome){.had_state = had_state, .marker_placed = marker_placed};
}

/* True when `it` is a tool_result whose output already ends with the
 * interrupt marker — i.e. bash appended its "[interrupted]" footer when
 * killed by user-Esc. Used to decide whether the post-dispatch break
 * needs an additional synthetic marker, since non-bash tools (read,
 * write, edit) finish normally without any footer and would otherwise
 * leave the next turn with no signal that the user aborted. */
static int tool_result_is_marked(const struct item *it)
{
    if (it->kind != ITEM_TOOL_RESULT || !it->output)
        return 0;
    size_t out_len = strlen(it->output);
    size_t marker_len = strlen(INTERRUPT_MARKER);
    if (out_len < marker_len)
        return 0;
    return strcmp(it->output + out_len - marker_len, INTERRUPT_MARKER) == 0;
}

/* Flush the same item slice to both append-only logs. The two share the
 * incremental [n_written..n_items) cursor design, so they're always
 * advanced together at each of the agent's commit points (after the user
 * message, after each round-trip, on the error/interrupt paths). Both
 * entry points no-op on a NULL log or when nothing new accumulated. */
static void flush_logs(struct transcript_log *tlog, struct session_log *slog,
                       const struct item *items, size_t n)
{
    transcript_log_append(tlog, items, n);
    session_log_append(slog, items, n);
}

/* Per-stream slice the event callback needs alongside render_ctx. */
struct event_ctx {
    struct render_ctx *r;
    struct turn *turn;
    /* Filled in from EV_DONE; -1/-1/-1 if the provider didn't report. */
    struct stream_usage usage;
};

/* How long after the last text byte we wait before surfacing the
 * "still alive" inline glyph. Shorter than the silent-cluster's
 * INLINE_TIMEOUT_MS because the inline glyph is non-disruptive — one
 * dim cell at the cursor's natural position, no paragraph break, no
 * disp bookkeeping perturbed — so we can show it sooner without the
 * risk of flicker between rapid deltas. */
#define TEXT_IDLE_TIMEOUT_MS 1500

/* How long a tool call must keep composing (name known, args still
 * streaming) before the busy spinner swaps "working..." for the named
 * "[tool] composing..." label. The spinner is already up either way —
 * only the text changes — so a short defer is enough to keep a fast call
 * from flashing working->composing->header; a genuinely long compose
 * (a big write/edit's args) still gets named. */
#define TOOL_COMPOSE_LABEL_MS 500

/* Adapter so md_renderer can emit through disp without knowing about it.
 * Content bytes go through disp_write so trailing-newline buffering works
 * uniformly. ANSI escapes (is_raw=1) bypass disp's trail/held bookkeeping
 * — otherwise an escape after a buffered \n would commit the held NL and
 * reset trail to 0, leaking an extra blank line out of the next
 * disp_block_separator. The escape goes ahead of the held NL on stdout
 * but is zero-width, so the visible result is identical. */
static void md_emit_to_disp(const char *bytes, size_t n, int is_raw, void *user)
{
    if (is_raw)
        fwrite(bytes, 1, n, stdout);
    else
        disp_write((struct disp *)user, bytes, n);
}

static int markdown_enabled(void)
{
    /* Hard gate, not a default: the markdown renderer drives the cursor
     * (retro-wrap via CSI sequences — see the cell-budget comment below),
     * which would pollute piped output, so non-TTY is always raw even
     * against an explicit markdown=1. On a TTY the setting is an
     * off-switch; its fixed default (on) lives in the registry. */
    if (!isatty(fileno(stdout)))
        return 0;
    return config_bool("markdown");
}

/* Cell budget for the markdown wrap engine. display_width() is the
 * content width we'd like, but the eager-wrap engine streams each
 * codepoint to the terminal and retro-wraps with CSI nD + CSI K
 * (markdown.c:wrap_break) — math that assumes the cursor sits one cell
 * past the last glyph. A glyph in the *physical* last column violates
 * that: terminals that defer the autowrap (xterm's pending-wrap,
 * libvterm's at_phantom) leave the cursor on the last column, not past
 * it, so the cursor-back lands one cell too far left and the erase eats
 * the previous word's last character. Reserve the last column so the
 * engine never fills it and the cursor model holds on every terminal.
 * Only bites when display_width() == term_width() (narrow terminals,
 * e.g. nvim's sidebar); the wide case is already capped well short.
 * term_width() is the raw physical edge, so this holds even on the
 * sub-20-column terminals display_width() floors away from. */
static int md_wrap_width(void)
{
    int w = display_width();
    int edge = term_width() - 1;
    return w < edge ? w : edge;
}

/* Whether to render reasoning/CoT deltas live in a dim block. Default
 * off because the volume can be large and most users want only the
 * model's final answer. Backend opt-in is separate (some servers only
 * stream reasoning when explicitly requested — see openrouter); this
 * just decides whether the deltas we receive get drawn. */
static int reasoning_visible(void)
{
    return config_bool("show_reasoning");
}

/* Format a token count in 1024-base: "412", "5.4k", "128k", "1.2M".
 * Powers-of-two are used because advertised context windows are typically
 * 32k/128k/256k (= 32×1024 etc.), so 1024-base produces the cleaner round
 * numbers users expect. < 0 → "?" (provider didn't report). */
static void format_tokens(char *buf, size_t buflen, long n)
{
    if (n < 0)
        snprintf(buf, buflen, "?");
    else if (n < 1024)
        snprintf(buf, buflen, "%ld", n);
    else if (n < 10L * 1024)
        snprintf(buf, buflen, "%.1fk", (double)n / 1024.0);
    else if (n < 1024L * 1024)
        snprintf(buf, buflen, "%ldk", (n + 512) / 1024);
    else if (n < 10L * 1024 * 1024)
        snprintf(buf, buflen, "%.1fM", (double)n / (1024.0 * 1024.0));
    else
        snprintf(buf, buflen, "%ldM", (n + 512L * 1024) / (1024L * 1024));
}

/* Resolve the context-window value used to render the "%" of context
 * used. Two sources, in order of precedence:
 *   1. HAX_CONTEXT_LIMIT — explicit user override (e.g. "256k"),
 *      always wins so the user can correct a bogus auto-detect.
 *   2. provider->context_limit — populated by a provider's startup
 *      probe (codex /models, openrouter /models, llama.cpp /props).
 *      Atomic since the probe runs on a background thread; 0 means
 *      "unknown" (no probe ran, hasn't completed yet, or failed) and
 *      the percentage display is hidden in that case. */
static long context_limit(const struct provider *p)
{
    long env = config_size("context_limit");
    if (env > 0)
        return env;
    long auto_v = atomic_load(&p->context_limit);
    return auto_v > 0 ? auto_v : 0;
}

/* Dim one-liner: "context 8.9k / 256k (3%) · out 595 · cached 2.7k", shown
 * once per user turn so multi-step tool runs collapse into a single summary
 * instead of bracketing every intermediate response.
 *
 * ctx and cached reflect the last response (= current window state — each
 * call's input subsumes the prior call's prefix, so the latest values are
 * the right snapshot). out is a running sum across the user turn's model calls,
 * answering "how many tokens did this prompt cost in generation". Each
 * value is -1 when the underlying counts weren't reported by the backend;
 * the section is then skipped rather than rendered with a misleading zero. */
static void display_usage(struct render_ctx *r, const struct provider *p, long ctx, long out,
                          long cached)
{
    int show_ctx = ctx >= 0;
    int show_out = out >= 0;
    int show_cached = cached > 0;
    if (!show_ctx && !show_out && !show_cached)
        return;

    struct disp *d = &r->disp;
    render_open_block(r);
    disp_raw(ANSI_DIM);

    const char *sep = "";
    char buf[32], limit_buf[32];
    if (show_ctx) {
        format_tokens(buf, sizeof(buf), ctx);
        disp_printf(d, "context %s", buf);
        long limit = context_limit(p);
        if (limit > 0) {
            format_tokens(limit_buf, sizeof(limit_buf), limit);
            disp_printf(d, " / %s (%ld%%)", limit_buf, ctx * 100 / limit);
        }
        sep = " · ";
    }
    if (show_out) {
        format_tokens(buf, sizeof(buf), out);
        disp_printf(d, "%sout %s", sep, buf);
        sep = " · ";
    }
    if (show_cached) {
        format_tokens(buf, sizeof(buf), cached);
        disp_printf(d, "%scached %s", sep, buf);
    }
    disp_raw(ANSI_RESET);
    disp_putc(d, '\n');
    fflush(stdout);
}

/* Per-stream side-channel hook: runs idle bookkeeping and returns the
 * cancel signal. Called from libcurl's progress callback (~1Hz) and
 * on every received chunk while the agent thread is parked inside
 * curl_easy_perform. Combines two responsibilities so the http layer
 * sees one tick instead of separate cancel/idle entry points.
 *
 * Idle behavior: when the model has emitted text and then gone quiet
 * for TEXT_IDLE_TIMEOUT_MS, drop a single dim glyph at the cursor's
 * current position via sticky-inline mode. Nothing else moves —
 * disp's held/trail counters stay as-is, the model's paragraph
 * layout is untouched, the markdown renderer's lookahead tail keeps
 * its buffered bytes. On text resume, spinner_hide's \b space \b
 * restores the cursor; the next byte the model emits overwrites the
 * cell that briefly held the glyph, so the resumed stream looks
 * identical to one that never paused. Sticky inline suppresses the
 * auto-transition to line mode that the silent cluster relies on —
 * any disp-bypassing \n here would desync the held/trail bookkeeping. */
static int agent_stream_tick(void *user)
{
    struct render_ctx *r = user;
    /* Retry countdown wins over idle detection: during a retry sleep the
     * model isn't streaming at all, so there's no idle-text window to
     * surface. Repaint each tick so the seconds count visibly shrinks. */
    if (r->retry_deadline_at) {
        update_retry_label(r);
    } else if (r->compose_at && monotonic_ms() - r->compose_at >= TOOL_COMPOSE_LABEL_MS) {
        /* A tool call has been composing past the defer threshold — swap
         * the busy spinner's "working..." for its name. Latch (compose_at
         * = 0) so we set the label once; on_event clears it when the
         * compose window ends. composing_active keeps a back-to-back batch
         * on a composing label instead of reverting per call. */
        spinner_set_label(r->spinner, r->compose_label);
        r->compose_at = 0;
        r->composing_active = 1;
    } else if (r->compose_end_at && monotonic_ms() - r->compose_end_at >= TOOL_COMPOSE_LABEL_MS) {
        /* A composing call's args ended and nothing followed within the
         * window — no next call in the batch, no done/dispatch — so the
         * model has stalled after finalizing args. Stop claiming it's
         * composing; revert to the generic busy label. */
        spinner_set_label(r->spinner, "working...");
        r->composing_active = 0;
        r->compose_end_at = 0;
    } else if (r->md && md_in_table(r->md)) {
        /* Buffering a table: bytes are arriving but nothing renders until
         * the whole grid lays out. Surface a labeled "composing..." line
         * spinner once the silent window crosses the idle threshold (a
         * fast table finalizes first and never shows it). render_text_chunk
         * keeps it up across the row deltas; this covers a mid-table stall
         * where no further delta arrives to re-issue it. */
        if (!r->table_composing && r->last_text_at &&
            monotonic_ms() - r->last_text_at >= TEXT_IDLE_TIMEOUT_MS)
            render_table_spinner_show(r);
    } else if (r->last_text_at && !r->idle_shown &&
               monotonic_ms() - r->last_text_at >= TEXT_IDLE_TIMEOUT_MS) {
        /* Inline glyph is only safe when:
         *
         *   - md is driving output (HAX_MARKDOWN=0 has no column
         *     source for the plain disp_write path, so the gate
         *     can't decide);
         *   - disp has no held newlines (md_cursor_col reads md's
         *     post-\n column == 0 after a hard \n, but disp defers
         *     trailing \n into r->disp.held instead of writing to
         *     stdout — so the real terminal cursor is still at the
         *     prior row's right edge, and the gate would pass even
         *     though pad + glyph would autowrap);
         *   - cursor + cells stays strictly left of term_width() (the
         *     real tty edge, not the soft-capped layout width) so the
         *     glyph never lands in the physical last column.
         *
         * Filling the last column arms the terminal's deferred autowrap
         * (xterm pending-wrap, libvterm at_phantom); spinner_hide's
         * \b-based erase then runs against that pending state, and
         * libvterm's BS leaves at_phantom set — the same off-by-one
         * that ate wrap characters. Reserving the column (strict <)
         * keeps the erase on solid ground. We trade the idle-alive
         * indicator for correctness in the unsafe cases; latch
         * idle_shown either way so the tick doesn't churn — the
         * next text byte moves last_text_at forward and re-arms
         * the idle window. */
        if (r->md && r->disp.held == 0) {
            /* Skip the leading-space cell when disp says the cursor
             * is already at column 0 or right after a space — the
             * model's last byte is its own breathing room, and an
             * extra pad would read as a stray double-space once the
             * glyph erases. */
            int pad = !r->disp.at_space_or_bol;
            int spinner_cells = 1 + pad;
            if (md_cursor_col(r->md) + spinner_cells < term_width())
                spinner_show_inline_text(r->spinner, pad);
        }
        r->idle_shown = 1;
    }
    return interrupt_requested();
}

static int on_event(const struct stream_event *ev, void *user)
{
    struct event_ctx *ec = user;
    struct render_ctx *r = ec->r;
    struct disp *d = &r->disp;

    /* Idle detection tracks "time since the last byte the user could
     * see". Stream-ending events close the window; streamed item seams
     * close it via render_stream_seam so long tool-args / post-reasoning
     * pauses use the full-size busy spinner instead of an inline glyph.
     * Reasoning deltas re-arm it from their own handler when reasoning is
     * visible. */
    if (ev->kind == EV_DONE || ev->kind == EV_ERROR)
        r->last_text_at = 0;
    /* Any event other than EV_RETRY itself means we're past the
     * backoff sleep — clear the countdown so per-event label updates
     * aren't fighting the tick. */
    if (ev->kind != EV_RETRY && r->retry_deadline_at) {
        r->retry_deadline_at = 0;
        spinner_set_label(r->spinner, "working...");
    }
    /* Drop any pending deferred "composing..." swap: only EV_TOOL_CALL_DELTA
     * (args still arriving) keeps it alive. EV_TOOL_CALL_END means the call
     * is fully composed, so a post-END stall must not let the tick flip the
     * busy spinner to "composing..." after the fact; every other event sets
     * its own label or ends the stream. EV_TOOL_CALL_START re-arms below. */
    if (ev->kind != EV_TOOL_CALL_DELTA)
        r->compose_at = 0;
    /* Cancel a pending revert-to-"working..." (armed at EV_TOOL_CALL_END):
     * the next call's START or the done event arriving means the model
     * didn't stall, so the batch stays on its composing label. END itself
     * arms it just below; only deltas otherwise leave it untouched. */
    if (ev->kind != EV_TOOL_CALL_DELTA && ev->kind != EV_TOOL_CALL_END)
        r->compose_end_at = 0;

    switch (ev->kind) {
    case EV_TEXT_DELTA:
        /* Strip the first delta's leading newlines, open RS_TEXT, feed —
         * shared with resume replay so both render identically. */
        render_text_delta(r, ev->u.text_delta.text, strlen(ev->u.text_delta.text));
        break;
    case EV_TOOL_CALL_START: {
        /* The model is composing a tool call: its name is known but the
         * args JSON still streams (long for a big write/edit). Name the
         * spinner so the otherwise-anonymous compose window says what's
         * coming — but defer the swap on the first call: leave the busy
         * spinner on "working..." and only arm compose_at/compose_label, so
         * the tick swaps in "[tool] composing..." once the window outlives
         * TOOL_COMPOSE_LABEL_MS. A fast call dispatches first with no
         * working->composing->header flicker.
         *
         * We track only the latest-started call (single compose_label, no
         * per-id state). For every provider in the supported set this names
         * the right call: an autoregressive model emits one call's tokens —
         * name then full args — before the next's, and OpenAI streams its
         * indexed tool_calls array one entry to completion before the next,
         * so the args streaming right now belong to the most recent START.
         * ("Parallel" tool calls run in parallel only at dispatch.) The
         * indexed-array shape does technically permit announcing several
         * names up front and backfilling one call's args later (START a,
         * START b, then a long DELTA a) — no backend we target emits that
         * order, but if one did the label could name a sibling call from the
         * same response for under a second. We accept that: it's cosmetic and
         * self-corrects when dispatch paints the real per-call headers, not
         * worth an id->name map to chase. */
        const char *name = ev->u.tool_call_start.name;
        if (name && *name)
            snprintf(r->compose_label, sizeof(r->compose_label), "[%s] composing...", name);
        if (r->composing_active && name && *name && r->state == RS_WAITING) {
            /* Mid-batch: a previous call in this stream already swapped the
             * spinner to a composing label and the busy line spinner is
             * still up (RS_WAITING, the normal state across a back-to-back
             * tool-call batch), so skip render_stream_seam's "working..."
             * reset — it would repaint "working..." for an instant before we
             * swap it back — and swap straight to this call's name. The
             * RS_WAITING guard means intervening text/reasoning (spinner
             * hidden) still falls through to render_stream_seam below, which
             * re-raises the spinner. */
            spinner_set_label(r->spinner, r->compose_label);
        } else {
            render_stream_seam(r);
            if (name && *name)
                r->compose_at = monotonic_ms();
        }
        break;
    }
    case EV_REASONING_ITEM:
        render_stream_seam(r);
        break;
    case EV_TOOL_CALL_END:
        /* Args for this call are finalized. If a composing label is on the
         * spinner, arm a deferred revert to "working...": the next call's
         * START or the done event normally lands within TOOL_COMPOSE_LABEL_MS
         * and cancels it (top of on_event), keeping a batch smooth; if the
         * model instead stalls after the args, the tick reverts so the label
         * stops claiming the tool is still composing. */
        if (r->composing_active)
            r->compose_end_at = monotonic_ms();
        break;
    case EV_TOOL_CALL_DELTA:
        /* No live display: tool calls render as a single block during
         * dispatch so parallel calls don't visually interleave. */
        break;
    case EV_REASONING_DELTA: {
        /* Flip the label even when reasoning is invisible — the
         * spinner still shows so the user knows the quiet pause is
         * the model reasoning, not the network. */
        spinner_set_label(r->spinner, "thinking...");
        const char *rt = ev->u.reasoning_delta.text;
        if (!r->show_reasoning || !rt || !*rt)
            break;
        render_transition(r, RS_REASONING);
        render_text_chunk(r, rt, strlen(rt));
        break;
    }
    case EV_RETRY: {
        /* Provider is about to back off before another attempt. Stash
         * the deadline so the tick can repaint with a live countdown;
         * paint immediately so there's no flicker between the prior
         * label and the first tick. attempt+1 is the *next* attempt
         * about to start. */
        r->retry_deadline_at = monotonic_ms() + ev->u.retry.delay_ms;
        r->retry_attempt = ev->u.retry.attempt + 1;
        r->retry_max = ev->u.retry.max_attempts;
        update_retry_label(r);
        break;
    }
    case EV_PROGRESS: {
        /* Prefill progress for this turn. Compute as
         * (processed - cache) / (total - cache) — the "work this turn
         * requires" view — so the percentage starts at 0% each turn
         * regardless of cache reuse. If the entire prompt was cached
         * (total == cache) there's no work to report; skip. The first
         * content/reasoning delta naturally overwrites the label via
         * the existing render_transition() paths. */
        long total = ev->u.progress.total;
        long cache = ev->u.progress.cache;
        long processed = ev->u.progress.processed;
        long denom = total - cache;
        if (denom <= 0)
            break;
        long num = processed - cache;
        if (num < 0)
            num = 0;
        int pct = (int)((num * 100) / denom);
        if (pct < 0)
            pct = 0;
        if (pct > 100)
            pct = 100;
        char buf[32];
        snprintf(buf, sizeof(buf), "processing... %d%%", pct);
        spinner_set_label(r->spinner, buf);
        break;
    }
    case EV_DONE:
        /* Stream ended cleanly. No state transition — agent_run's
         * post-stream path closes whatever was open. */
        ec->usage = ev->u.done.usage;
        fflush(stdout);
        break;
    case EV_ERROR:
        /* Full close before drawing the error block. render_open_block's
         * RS_IDLE transition flushes md's tail if RS_TEXT was open, so
         * partial pre-error text appears just above the error line. */
        render_open_block(r);
        disp_raw(ANSI_RED);
        disp_printf(d, "[error: %s]", ev->u.error.message);
        disp_raw(ANSI_RESET);
        disp_putc(d, '\n');
        fflush(stdout);
        break;
    }

    turn_on_event(ev, ec->turn);
    return 0;
}

/* Cursor visibility tracks "user is being asked to type": shown only
 * around input_readline, hidden everywhere else (slash commands,
 * streaming, tool dispatch). The spinner glyph is the "we're alive"
 * indicator while hidden. Restoration on abnormal exit lives in
 * interrupt.c's restore_tty_only(); these helpers handle the normal
 * loop. Gate on BOTH stdin and stdout being TTYs to match
 * interrupt_init()'s condition — otherwise atexit/signal restore
 * isn't installed, and a piped-stdin run on a TTY stdout would leak
 * a hidden-cursor state to the parent shell after EOF or signal. */
static int cursor_supported(void)
{
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

static void cursor_show(void)
{
    if (!cursor_supported())
        return;
    fputs(ANSI_CURSOR_SHOW, stdout);
    fflush(stdout);
}

static void cursor_hide(void)
{
    if (!cursor_supported())
        return;
    fputs(ANSI_CURSOR_HIDE, stdout);
    fflush(stdout);
}

/* Indirect references into the live agent_session so the Ctrl-T
 * callback always sees the latest values — `items` is reassigned by
 * xrealloc as the vector grows, and `n_items` advances with every
 * append. Holding the addresses sidesteps the moving target. `sys` is
 * fixed for the session's lifetime, but routing it through the same
 * indirection keeps the three fields uniform.
 *
 * Lifetime: instances live on agent_run's stack frame and are
 * registered with the input editor for the duration of that call.
 * input_free must run before agent_run returns (it does — see the
 * cleanup at the end of agent_run), otherwise `input` would outlive
 * the storage and a stray Ctrl-T would dereference a dead frame. */
struct transcript_view {
    const char *const *sys_ref;
    struct item *const *items_ref;
    const size_t *n_items_ref;
    /* Tools and n_tools are stable for the session's lifetime (set in
     * agent_session_init, freed in _destroy, not touched by /new), so
     * they're held by value — no indirection needed. */
    const struct tool_def *tools;
    size_t n_tools;
};

static void show_transcript_cb(void *user)
{
    struct transcript_view *v = user;
    const char *pager = getenv("PAGER");
    if (!pager || !*pager)
        pager = "less -R";
    /* spawn_pipe_open shields the parent from terminal-generated
     * SIGINT/SIGQUIT (so Ctrl-C in the pager exits the pager, not
     * hax) and from SIGPIPE on the fputs path (so quitting the pager
     * early gives EPIPE rather than killing hax). The child sees all
     * three at default disposition, so less behaves normally. */
    struct spawn_pipe sp;
    if (spawn_pipe_open(&sp, pager) < 0)
        return;
    transcript_render(sp.w, *v->sys_ref, v->tools, v->n_tools, *v->items_ref, *v->n_items_ref);
    spawn_pipe_close(&sp);
}

/* Caller is expected to have already emitted the leading blank-line
 * gap (slash_dispatch does this for /new; agent_run does it at startup
 * before the first call). The banner itself is just two output rows so
 * it composes cleanly with whatever surrounded the call. */
void agent_print_banner(const struct provider *p, const struct agent_session *s)
{
    const char *bar = ANSI_CYAN "▌" ANSI_FG_DEFAULT;
    if (s->reasoning_effort)
        printf("%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM "› %s · %s · %s" ANSI_BOLD_OFF "\n",
               bar, p->name ? p->name : "?", s->model, s->reasoning_effort);
    else
        printf("%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM "› %s · %s" ANSI_BOLD_OFF "\n", bar,
               p->name ? p->name : "?", s->model);
    printf("%s " ANSI_DIM "ctrl-d quit · try /help" ANSI_BOLD_OFF "\n", bar);
}

void agent_new_conversation(struct agent_state *st)
{
    agent_session_reset(st->sess);
    transcript_log_reset(st->tlog, st->sess->sys, st->sess->tools, st->sess->n_tools);
    /* Rotate to a fresh session file so the cleared conversation doesn't
     * keep appending to the prior session's record. */
    session_log_reset(st->slog);
    /* The model loses access to anything not in the conversation history,
     * so any preserved bash temp files referenced by old turns become
     * unreachable garbage. Drop them now rather than letting them sit in
     * /tmp until process exit (or longer if the user kills the process). */
    bash_cleanup_tempfiles();
    agent_print_banner(st->provider, st->sess);
}

/* Render the standalone "[interrupted]" marker as its own dim out-of-band
 * block. Shared by the live interrupt path (agent_run's turn-interrupted
 * branch) and history replay, so a resumed interrupt looks the same as a
 * fresh one rather than as plain assistant text. */
static void render_interrupt_marker(struct render_ctx *r)
{
    render_open_block(r);
    disp_raw(ANSI_DIM);
    disp_printf(&r->disp, "%s", INTERRUPT_MARKER);
    disp_raw(ANSI_RESET);
    disp_putc(&r->disp, '\n');
    fflush(stdout);
}

/* Echo a stored user message exactly as the live editor repaints a
 * submitted one — the magenta "▌ " stripe + magenta wrapped body — so a
 * replayed prompt is indistinguishable from one just typed. The editor
 * writes directly to stdout (bypassing disp), ending at column 0 of a
 * fresh row, so we resync disp afterward the same way agent_run does after
 * input_readline. */
static void replay_user_echo(struct render_ctx *r, const char *text)
{
    render_open_block(r); /* one blank line above, cursor at column 0 */
    /* input_display_cols(), not term_width(): match the editor's wrap width
     * exactly (display_width() clamped to the tty, HAX_DISPLAY_WIDTH-aware)
     * so a replayed prompt wraps identically to a freshly typed one. */
    input_render_user_message(text ? text : "", text ? strlen(text) : 0, input_display_cols());
    r->disp.trail = 1;
    r->disp.held = 0;
    r->disp.at_space_or_bol = 1;
}

/* Feed a stored assistant message or reasoning blob into the markdown
 * stream exactly as the live path would, so wrapping and block spacing
 * come out identical. NULL/empty is a no-op — opaque (Codex) reasoning has
 * no reasoning_text and replays as nothing, matching the live display. */
static void replay_text(struct render_ctx *r, enum render_state target, const char *text)
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
            md_reset(r->md, md_wrap_width());
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

/* Replay a stored assistant message. A synthetic interrupt marker is split
 * off and rendered through the dim out-of-band block, matching live: a
 * standalone "[interrupted]" (aborted before output) shows only the dim
 * block, and a partial response with "\n[interrupted]" appended (aborted
 * mid-text) shows the partial as normal markdown then the dim block. The
 * marker is recognized only at a line boundary (whole message, or right
 * after a \n) — its exact stored forms — so a legitimate response that
 * merely ends with the literal "[interrupted]" is left as normal text. */
static void replay_assistant(struct render_ctx *r, const char *text)
{
    if (!text || !*text)
        return;
    size_t len = strlen(text);
    size_t mlen = strlen(INTERRUPT_MARKER);
    if (len >= mlen && strcmp(text + len - mlen, INTERRUPT_MARKER) == 0) {
        size_t before = len - mlen; /* bytes before the marker */
        if (before == 0 || text[before - 1] == '\n') {
            if (before > 1) { /* a partial response precedes "\n[interrupted]" */
                char *partial = xasprintf("%.*s", (int)(before - 1), text);
                replay_text(r, RS_TEXT, partial);
                free(partial);
            }
            render_interrupt_marker(r);
            return;
        }
    }
    replay_text(r, RS_TEXT, text);
}

/* Replay the last user turn — the final user message plus every turn the
 * model produced in response — through the live display pipeline, so
 * resuming looks like the conversation never scrolled away rather than
 * dropping the user at a bare prompt. Assistant text and (when shown)
 * reasoning render at full fidelity; tool calls collapse to dim one-line
 * headers, since their output previews can't be rebuilt from stored items
 * and the tools can't be safely re-run. Earlier history stays one Ctrl-T
 * away, summarized by the dim rule's count.
 *
 * Anchored on the last ITEM_USER_MESSAGE, not the last TURN_BOUNDARY: one
 * user prompt can span several round-trips (turns), and we want them all.
 * Interactive-only — gated on both stdin and stdout being TTYs (same as
 * cursor_supported()), so a non-interactive run (`printf … | hax --resume`,
 * or any piped stdin/stdout) renders nothing extra before processing input. */
static void replay_user_turn(struct render_ctx *r, const struct agent_session *s)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return;

    size_t anchor = 0;
    int found = 0;
    size_t earlier = 0;
    for (size_t i = s->n_items; i-- > 0;) {
        if (s->items[i].kind != ITEM_USER_MESSAGE)
            continue;
        if (!found) {
            anchor = i;
            found = 1;
        } else {
            earlier++;
        }
    }

    /* The startup banner and the /resume picker both write directly to
     * stdout, leaving the cursor at column 0 of a fresh row but disp's
     * held/column bookkeeping stale — resync those. The trailing-newline
     * count differs by caller (the /resume slash command leaves an extra
     * leading blank line below the echoed command), so the caller sets
     * r->disp.trail before invoking us and we trust it for the rule's
     * separator. */
    r->disp.held = 0;
    r->disp.at_space_or_bol = 1;

    render_open_block(r);
    disp_raw(ANSI_DIM);
    if (earlier > 0)
        disp_printf(&r->disp, "── resumed · %zu earlier message%s · ctrl-t for full history ──",
                    earlier, earlier == 1 ? "" : "s");
    else
        disp_printf(&r->disp, "── resumed · ctrl-t for full history ──");
    disp_raw(ANSI_RESET);
    disp_putc(&r->disp, '\n');

    if (found) {
        for (size_t i = anchor; i < s->n_items; i++) {
            const struct item *it = &s->items[i];
            switch (it->kind) {
            case ITEM_USER_MESSAGE:
                replay_user_echo(r, it->text);
                break;
            case ITEM_ASSISTANT_MESSAGE:
                replay_assistant(r, it->text);
                break;
            case ITEM_REASONING:
                if (r->show_reasoning)
                    replay_text(r, RS_REASONING, it->reasoning_text);
                break;
            case ITEM_TOOL_CALL:
                /* RS_CLUSTER groups consecutive calls under one block
                 * separator and lets them stack tight (the next non-tool
                 * item transitions out cleanly). */
                render_transition(r, RS_CLUSTER);
                render_collapsed_tool_call(r, it);
                break;
            case ITEM_TOOL_RESULT:
            case ITEM_TURN_BOUNDARY:
                break;
            }
        }
    }

    /* Land at column 0 with one committed trailing newline, so the prompt
     * that follows is separated by a clean blank line. md_flush leaves the
     * last row open (no trailing \n), so terminate it explicitly; then
     * commit any held newline. This makes the slash path's `trail = 1`
     * reset (after /resume) and the startup loop's separator both correct. */
    render_transition(r, RS_IDLE);
    if (r->disp.trail == 0 && r->disp.held == 0)
        disp_putc(&r->disp, '\n');
    disp_emit_held(&r->disp);
    fflush(stdout);
}

void agent_resume_session(struct agent_state *st, const char *path)
{
    struct agent_session *s = st->sess;
    struct item *loaded = NULL;
    size_t nl = 0;
    if (session_load(path, st->provider->name, s->model, &loaded, &nl, NULL) != 0 || nl == 0) {
        free(loaded);
        ui_error("could not read session");
        /* The error line is now the last thing printed, on its own fresh
         * row. /resume set trail = 2 for the picker that's now moot; correct
         * it to the post-error state (one trailing newline) so the next
         * prompt still gets its blank-line separation. */
        st->r->disp.trail = 1;
        return;
    }

    /* Swap the loaded conversation in for the current one. */
    for (size_t i = 0; i < s->n_items; i++)
        item_free(&s->items[i]);
    free(s->items);
    s->items = loaded;
    s->n_items = nl;
    s->cap_items = nl;

    /* Continue the resumed file rather than the prior one, and re-key the
     * transcript mirror to the restored history (reset writes the header,
     * the append replays the items). */
    session_log_close(st->slog);
    st->slog = session_log_resume(path, st->provider->name, s->model, s->reasoning_effort, nl);
    transcript_log_reset(st->tlog, s->sys, s->tools, s->n_tools);
    transcript_log_append(st->tlog, s->items, s->n_items);

    /* Old bash temp files referenced by the prior conversation are now
     * unreachable, same as on /new. */
    bash_cleanup_tempfiles();
    replay_user_turn(st->r, s);
}

int agent_run(struct provider *p, const struct hax_opts *opts)
{
    struct agent_session sess;
    if (agent_session_init(&sess, p, opts) < 0)
        return 1;

    /* Resume: load a prior conversation into history before anything else
     * touches it, so the Ctrl-T view, the HAX_TRANSCRIPT mirror, and the
     * session log all see the restored items from the start. An unreadable
     * file is fatal — silently starting fresh would run against the wrong
     * history. An empty-but-readable session (e.g. truncated by a crash)
     * loads as zero items and just resumes empty, continuing that file. */
    size_t n_resumed = 0;
    if (opts->resume_path) {
        struct item *loaded = NULL;
        size_t nl = 0;
        if (session_load(opts->resume_path, p->name, sess.model, &loaded, &nl, NULL) != 0) {
            hax_err("could not resume session '%s'", opts->resume_path);
            agent_session_free(&sess);
            return 1;
        }
        sess.items = loaded;
        sess.n_items = nl;
        sess.cap_items = nl;
        n_resumed = nl;
    }

    putchar('\n');
    agent_print_banner(p, &sess);
    /* The single bag of rendering state threaded through every render
     * call. disp is embedded (same lifetime as agent_run's frame), so
     * md_emit_to_disp's user pointer is &r.disp; spinner / md are
     * opaque handles owned here and freed below. */
    struct render_ctx r = {.disp = {.trail = 1, .at_space_or_bol = 1},
                           .show_reasoning = reasoning_visible()};
    r.spinner = spinner_new("working...");
    r.md = markdown_enabled() ? md_new(md_emit_to_disp, &r.disp, md_wrap_width()) : NULL;
    /* On a --continue/--resume startup, replay the last user turn through
     * the live pipeline (same as /resume mid-session). Needs r/md ready,
     * so it lands here rather than right after the banner — nothing prints
     * in between, so it still reads as part of the startup sequence. */
    if (n_resumed > 0)
        replay_user_turn(&r, &sess);
    struct input *input = input_new();
    input_history_open_default(input);
    struct transcript_view tv = {
        /* Point at session fields so the Ctrl-T callback always sees
         * the latest values — the items vector grows via xrealloc so
         * its address changes, and these are read-only views. The cast
         * appeases C's lack of implicit multi-level const conversion. */
        .sys_ref = (const char *const *)&sess.sys,
        .items_ref = &sess.items,
        .n_items_ref = &sess.n_items,
        .tools = sess.tools,
        .n_tools = sess.n_tools,
    };
    input_set_transcript_cb(input, show_transcript_cb, &tv);
    /* HAX_TRANSCRIPT — append-only mirror of the Ctrl-T view. NULL when
     * the env var is unset; all transcript_log_* entry points are
     * NULL-safe so the call sites don't need a guard. */
    struct transcript_log *tlog = transcript_log_open(sess.sys, sess.tools, sess.n_tools);
    /* Aggregate handed to slash handlers so they can mutate live state
     * without each one taking a separate argument. Pointers into stack
     * frames here — agent_state never outlives agent_run. The session log
     * lives on `state` (not a separate local) because /resume swaps it
     * mid-run via agent_resume_session — every later use reads state.slog
     * so it tracks that swap instead of dangling on the closed handle. */
    struct agent_state state = {.sess = &sess, .provider = p, .tlog = tlog, .r = &r};
    /* Append-only session record. Resuming continues the same file (so
     * the restored items aren't re-written); otherwise a fresh file is
     * begun. NULL when persistence is disabled — all entry points are
     * NULL-safe. */
    state.slog = opts->resume_path ? session_log_resume(opts->resume_path, p->name, sess.model,
                                                        sess.reasoning_effort, n_resumed)
                                   : session_log_open(p->name, sess.model, sess.reasoning_effort);
    /* Mirror restored history into the HAX_TRANSCRIPT log — its header
     * was just written by _open; the items follow so the mirror matches
     * the live conversation. */
    if (n_resumed > 0)
        transcript_log_append(tlog, sess.items, sess.n_items);
    /* Initialize once — captures the canonical termios baseline and starts
     * the watcher thread. Idempotent; safe even when stdin/stdout aren't
     * ttys (becomes a no-op in that case). */
    interrupt_init();

    const char *prompt = locale_have_utf8() ? PROMPT_UTF8 : PROMPT_ASCII;

    /* The render state in r (state + cluster sub-state) lives across
     * the inner loop so RS_CLUSTER can span consecutive silent tool
     * calls (read/grep/find...) without intervening blank lines.
     * End-of-user-turn cleanup unconditionally transitions back to
     * RS_IDLE, so leftover state from a prior user turn is impossible
     * by construction. */

    for (;;) {
        disp_block_separator(&r.disp);
        cursor_show();
        char *line = input_readline(input, prompt);
        cursor_hide();
        if (!line) {
            putchar('\n');
            break;
        }
        if (!*line) {
            free(line);
            continue;
        }

        /* Slash commands run locally (clear history, show help, ...) and
         * never reach the model. They join up-arrow recall for the current
         * session but aren't persisted: replaying yesterday's /new or
         * /usage in a fresh session is pointless and only dilutes the
         * stored prompt history. Lines that look like commands but aren't
         * (e.g. "/tmp/foo" — the dispatcher's bareword check) return
         * SLASH_NOT_A_COMMAND and fall through to the regular model path
         * below. */
        /* Slash output bypasses disp, so reset the trail to the state the
         * common case leaves: cursor at column 0 one newline below the
         * echoed command. Set it before dispatch so a handler that ends in
         * a different cursor state (e.g. /resume's full-screen picker) can
         * override it. */
        r.disp.trail = 1;
        struct slash_ctx sctx = {.state = &state};
        if (slash_dispatch(line, &sctx) != SLASH_NOT_A_COMMAND) {
            input_history_add_session(input, line);
            free(line);
            continue;
        }

        input_history_add(input, line);

        /* Mark the turn boundary just before the user message, not just
         * before the model request that consumes it. The transcript
         * renderer treats a TURN_BOUNDARY as a start-of-turn rule;
         * placing it ahead of the user message puts the first round-trip's
         * header above the triggering user input, so the prompt and the
         * response it produced read as one group. The inner loop's
         * subsequent iterations insert their own boundaries for follow-up
         * round-trips after tool dispatch. */
        agent_session_add_user(&sess, line);
        free(line);
        /* Flush the prompt to the log immediately, before we hand
         * control to the provider. If the stream hangs or the process
         * is killed, the user prompt that triggered the in-flight call
         * is preserved on disk for post-mortem reading. */
        flush_logs(tlog, state.slog, sess.items, sess.n_items);
        /* input_readline left the cursor at column 0 of a fresh row. */
        r.disp.trail = 1;

        /* Aggregated across every model call this user turn produces.
         * ctx and cached track the latest reported value (= current window
         * state, since each call's input subsumes the prior call's prefix);
         * out is a running sum so the summary reflects total tokens
         * generated in response to this prompt. -1 means "no call reported
         * this number yet". */
        long user_turn_ctx = -1, user_turn_out = -1, user_turn_cached = -1;
        int user_turn_errored = 0;
        int user_turn_interrupted = 0;

        /* Arm the watcher for the duration of the inner loop — Esc from
         * here on aborts the stream or running tool. Cleared first so a
         * stray Esc from a previous user turn (e.g. user typed Esc during
         * readline editing) doesn't auto-cancel this one. */
        interrupt_clear();
        interrupt_arm();
        /* Keep the machine from idling to sleep for the duration of the
         * inner loop (streaming + tool dispatch), so an unattended long
         * run survives the idle timer. Released alongside interrupt_disarm
         * below — there's no human-approval wait in this loop to hold it
         * across. Gated by the keep_awake config key; no-op when off. */
        keepawake_acquire();
        /* The first iteration consumes the boundary that was placed
         * with the user message above; subsequent iterations (when the
         * model called tools and we loop back for the next round-trip)
         * insert their own. */
        int first_inner = 1;
        for (;;) {
            if (!first_inner) {
                /* Inserted before ctx is built so the items_append's
                 * potential xrealloc can't dangle ctx.items, and so
                 * this call's n_items already reflects it (providers
                 * skip ITEM_TURN_BOUNDARY in their item-translation
                 * switch). */
                agent_session_add_boundary(&sess);
                /* Same rationale as the post-add_user flush above:
                 * land the new turn rule on disk before we wait on
                 * the next stream call. */
                flush_logs(tlog, state.slog, sess.items, sess.n_items);
            }
            first_inner = 0;
            struct context ctx = agent_session_context(&sess);

            render_stream_begin(&r);

            if (r.md)
                md_reset(r.md, md_wrap_width());
            struct turn t;
            turn_init(&t);
            r.disp.saw_text = 0;
            struct event_ctx ec = {.r = &r, .turn = &t, .usage = {-1, -1, -1}};
            /* agent_stream_tick combines cancel (Esc) with idle
             * detection: ~1Hz from libcurl's progress callback inside
             * curl_easy_perform, plus on every received chunk. */
            p->stream(p, &ctx, sess.model, on_event, &ec, agent_stream_tick, &r);

            if (t.error) {
                /* Mid-stream error (network drop, "length"/"content_filter"
                 * truncation, provider parse failure, ...). Preserve the
                 * partial assistant text in conversation history so the
                 * user can ask the model to "continue" on the next turn
                 * without losing what was already streamed. The on-screen
                 * "[error: ...]" line was already drawn by on_event;
                 * absorb_aborted_turn shapes what lands in history. */
                struct absorb_outcome o = absorb_aborted_turn(&sess, &t);
                if (o.had_state && !o.marker_placed) {
                    /* Tag the gap when state was streamed but nothing
                     * in the absorbed batch could carry the marker —
                     * e.g. text flushed by EV_TOOL_CALL_START (so
                     * in_text=0 by error time) with the pending
                     * tool_call never finalized. Without this, the
                     * model would see a complete-looking assistant
                     * response with no signal it was cut short.
                     *
                     * Skip when had_state=0 (pre-stream 500/401/429
                     * after retries): there's no partial work to
                     * preserve, and synthesizing a fake assistant
                     * "[interrupted]" would mislead the model on the
                     * user's next prompt. The on-screen "[error: ...]"
                     * line is the user-facing signal. */
                    items_append(&sess.items, &sess.n_items, &sess.cap_items,
                                 (struct item){
                                     .kind = ITEM_ASSISTANT_MESSAGE,
                                     .text = xstrdup(INTERRUPT_MARKER),
                                 });
                }
                /* Land the partial state on disk now — the success
                 * path's later append doesn't run on this branch and
                 * post-mortem readers want to see how far we got. */
                flush_logs(tlog, state.slog, sess.items, sess.n_items);
                user_turn_errored = 1;
                break;
            }

            /* Settle before deciding what to do with the response — Esc
             * pressed in the last ~50ms of the stream may still be in
             * the classifier's CSI/SS3-vs-bare window, and we must not
             * dispatch tools (or send another request) on a flag that's
             * about to flip. */
            interrupt_settle();
            int interrupted = interrupt_requested();

            if (ec.usage.input_tokens >= 0 && ec.usage.output_tokens >= 0)
                user_turn_ctx = ec.usage.input_tokens + ec.usage.output_tokens;
            if (ec.usage.output_tokens >= 0)
                user_turn_out = (user_turn_out < 0 ? 0 : user_turn_out) + ec.usage.output_tokens;
            if (ec.usage.cached_tokens >= 0)
                user_turn_cached = ec.usage.cached_tokens;

            if (interrupted) {
                /* User pressed Esc. Same shape-history-as-aborted dance
                 * as the EV_ERROR path; additionally, when nothing
                 * could carry the marker, drop a standalone one so
                 * the model sees the cancel signal on the next turn.
                 * Esc adds it unconditionally on no-marker (unlike
                 * EV_ERROR's had_state gate) — user cancellation
                 * deserves explicit acknowledgement even if no
                 * deltas had arrived yet ("Esc before any output"
                 * and "only normally-flushed text completed" cases). */
                struct absorb_outcome o = absorb_aborted_turn(&sess, &t);
                if (!o.marker_placed) {
                    items_append(&sess.items, &sess.n_items, &sess.cap_items,
                                 (struct item){
                                     .kind = ITEM_ASSISTANT_MESSAGE,
                                     .text = xstrdup(INTERRUPT_MARKER),
                                 });
                }
                user_turn_interrupted = 1;
                break;
            }

            /* Normal completion: absorb without any marker, decide
             * whether to loop for tool dispatch. */
            size_t n_before;
            int had_tool_call;
            agent_session_absorb(&sess, &t, &n_before, &had_tool_call);
            turn_reset(&t);

            if (!had_tool_call)
                break;

            /* Execute tool calls just added — render header + output as
             * one block per call so parallel calls don't interleave. The
             * spinner runs on the line between header and output so a
             * slow tool still gives the user a "still working" signal;
             * spinner_hide erases that line and tool output writes there
             * in its place. Esc partway through the batch flips remaining
             * calls to a synthesized "[interrupted]" result so the
             * conversation stays well-formed; settle first so a
             * fast-returning tool doesn't race past a pending \x1b in
             * the classifier. */
            size_t current_end = sess.n_items;
            for (size_t i = n_before; i < current_end; i++) {
                if (sess.items[i].kind != ITEM_TOOL_CALL)
                    continue;
                interrupt_settle();
                struct item result;
                if (sess.n_tools == 0) {
                    /* --raw advertised no tools; refuse to run anything
                     * the provider returned anyway. Same gate exists in
                     * oneshot.c — local execution must not be reachable
                     * from a malformed or malicious backend response. */
                    render_transition(&r, RS_IDLE);
                    result = dispatch_tool_refused(&r, &sess.items[i]);
                } else if (interrupt_requested()) {
                    render_transition(&r, RS_IDLE);
                    result = dispatch_tool_skipped(&r, &sess.items[i]);
                } else {
                    result = dispatch_tool_call(&r, &sess.items[i]);
                }
                items_append(&sess.items, &sess.n_items, &sess.cap_items, result);
            }

            /* Round-trip complete: assistant text, tool calls, and all
             * their results are in sess.items, so flush the slice into
             * the HAX_TRANSCRIPT log. Doing this *inside* the inner
             * loop (rather than only at the bottom) means a hung
             * follow-up stream, a crash mid-chain, or a SIGKILL leaves
             * the prompt + completed tool output on disk for
             * post-mortem reading — the main reason HAX_TRANSCRIPT
             * exists. CALL/RESULT pairing is intact so the renderer's
             * lookahead works. */
            flush_logs(tlog, state.slog, sess.items, sess.n_items);

            /* Esc fired during or just after this batch. Stop the inner
             * loop without another model call, and ensure history carries
             * a marker — bash appends its own "[interrupted]" footer when
             * killed, but read/write/edit return clean results that
             * would otherwise hide the abort from the next turn. Settle
             * first so we don't race past a pending \x1b. */
            interrupt_settle();
            if (interrupt_requested()) {
                if (sess.n_items == 0 || !tool_result_is_marked(&sess.items[sess.n_items - 1])) {
                    items_append(&sess.items, &sess.n_items, &sess.cap_items,
                                 (struct item){
                                     .kind = ITEM_ASSISTANT_MESSAGE,
                                     .text = xstrdup(INTERRUPT_MARKER),
                                 });
                }
                user_turn_interrupted = 1;
                break;
            }
        }
        interrupt_disarm();
        keepawake_release();

        /* Close any open render state before post-user-turn output — a
         * still-running cluster spinner racing with notify_attention's
         * OSC-9 would corrupt the escape sequence. */
        render_transition(&r, RS_IDLE);

        /* Catch-all flush. The per-round-trip append above covers tool
         * paths; this one picks up the no-tool exit (assistant message
         * only) and the synthesized-results interrupt path. Idempotent
         * when there are no new items — _append no-ops on n_items <= n_written. */
        flush_logs(tlog, state.slog, sess.items, sess.n_items);

        if (user_turn_interrupted)
            render_interrupt_marker(&r);

        if (!user_turn_errored)
            display_usage(&r, p, user_turn_ctx, user_turn_out, user_turn_cached);

        /* Ping the terminal so the user gets a notification / dock
         * bounce when hax is back to idle. Skipped on Esc-interrupt
         * since the user just pressed a key — they're already at the
         * terminal. Errored user turns still notify: the user needs to
         * know the request bounced. */
        if (!user_turn_interrupted)
            notify_attention();
    }

    /* On the way out, tell the user how to get back: the session id (NULL
     * for an empty or persistence-disabled run). The Ctrl-D path already
     * emitted a newline, so this lands on its own line under the prompt. */
    const char *hint = session_log_resume_hint(state.slog);
    if (hint)
        ui_note("resume with: hax --resume=%s", hint);

    spinner_free(r.spinner);
    input_free(input);
    if (r.md)
        md_free(r.md);
    transcript_log_close(tlog);
    session_log_close(state.slog);
    agent_session_free(&sess);
    return 0;
}
