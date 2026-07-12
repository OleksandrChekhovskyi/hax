/* SPDX-License-Identifier: MIT */
#include "agent.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "agent_dispatch.h"
#include "catalog.h"
#include "compact.h"
#include "config.h"
#include "file_mention.h"
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

/* Tally one tool invocation for /session. The per-type slot keys on the
 * registry's static name (via find_tool) rather than the item-owned
 * string, which compaction can free while stats live on; unregistered
 * names (refused/unknown calls) still count toward the total. */
static void stats_count_tool_call(struct session_stats *st, const char *name)
{
    st->tool_calls++;
    const struct tool *tl = name ? find_tool(name) : NULL;
    if (!tl)
        return;
    for (size_t i = 0; i < SESSION_STATS_MAX_TOOLS; i++) {
        if (st->tools[i].name == tl->def.name) {
            st->tools[i].count++;
            return;
        }
        if (!st->tools[i].name) {
            st->tools[i].name = tl->def.name;
            st->tools[i].count = 1;
            return;
        }
    }
}

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
static struct absorb_outcome absorb_aborted_turn(struct agent_session *sess, struct turn *t,
                                                 struct session_stats *stats)
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
            /* The model issued this call even though the abort keeps it
             * from dispatching — /session's tool-call count includes it,
             * same as the dispatch loop counts refused/skipped calls. */
            if (stats)
                stats_count_tool_call(stats, sess->items[i].tool_name);
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

/* Record a mid-turn user abort: append the [interrupted] marker as a synthetic
 * assistant message unless the last item already carries it (a tool result
 * marked at the abort boundary), so the next turn and the transcript both see
 * the model was cut short. */
static void append_interrupt_marker(struct agent_session *s)
{
    if (s->n_items == 0 || !tool_result_is_marked(&s->items[s->n_items - 1]))
        items_append(
            &s->items, &s->n_items, &s->cap_items,
            (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup(INTERRUPT_MARKER)});
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
    /* Filled in from EV_DONE; all fields -1 if the provider didn't report. */
    struct stream_usage usage;
};

/* How long a table must keep buffering with no visible output before
 * the "composing..." spinner surfaces. A fast table finalizes within
 * the window and never shows it; ordinary inter-token gaps stay well
 * under it. Pauses in ordinary streaming text deliberately show no
 * indicator at all — the arriving text is its own progress signal,
 * and a spinner popping in and out between chunks reads as flicker. */
#define TABLE_IDLE_TIMEOUT_MS 1500

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

double agent_session_spend(const struct session_stats *t, const struct provider *p,
                           const char *model, int *approx)
{
    double seg = spend_estimate(&t->spend, p, model);
    double est = t->est_cost + (seg > 0 ? seg : 0);
    /* Approximate = any inexact component: an estimate contributed, real
     * usage sits unpriced in the open segment (whether or not it could be
     * estimated), or a settle had to drop unpriceable tokens. A reported
     * subtotal must never display as an exact grand total. */
    if (approx)
        *approx = est > 0 || spend_has_tokens(&t->spend) || t->est_dropped;
    return t->spend.reported + est;
}

/* Close the open pricing segment: price its tokens at `model`'s catalog
 * rates (the model the segment accumulated under — the *outgoing* one on a
 * switch), fold the result into est_cost, and reset the segment counters.
 * Runs only when a switch actually changes the rates: an effort-only or
 * failed settings change must leave the segment open, because tokens that
 * can't be priced at settle time (catalog fetch not landed yet) are
 * dropped rather than guessed at foreign rates — est_dropped then keeps
 * the undercounting total marked approximate for the rest of the session. */
static void agent_stats_settle(struct agent_state *st, const char *model)
{
    struct session_stats *t = &st->stats;
    double seg = spend_estimate(&t->spend, st->provider, model);
    if (seg > 0)
        t->est_cost += seg;
    else if (spend_has_tokens(&t->spend))
        t->est_dropped = 1;
    t->spend = (struct spend_totals){.reported = t->spend.reported};
}

/* Dim per-user-turn stats line: "context 8.9k / 256k (3%) · 42s · $0.042",
 * shown once per user turn so multi-step tool runs collapse into a single
 * summary instead of bracketing every intermediate response. Distinct from
 * /usage (the provider's account report) and /session (this session's
 * totals) — this is the ambient per-turn view.
 *
 * ctx and cached reflect the last response (= current window state — each
 * call's input subsumes the prior call's prefix, so the latest values are
 * the right snapshot). out is a running sum across the user turn's model
 * calls. elapsed_ms is this user turn's wall time; spend is the *session's*
 * cumulative cost — provider-reported, plus the catalog estimate for
 * responses that reported none (agent_session_spend), the latter marking
 * the figure "~$". Field selection and formatting live in
 * format_stats_segments, shared with oneshot's exit summary.
 *
 * Segments wrap at the " · " seams against the content width, so a narrow
 * terminal reflows between fields instead of the terminal hard-wrapping
 * mid-number. Width is sampled at print time; the line is scrollback, so
 * there's no post-print reflow obligation. */
static void display_stats_line(struct render_ctx *r, const struct provider *p, const char *model,
                               long ctx, long out, long cached, long elapsed_ms,
                               const struct session_stats *stats)
{
    int approx = 0;
    double spend = agent_session_spend(stats, p, model, &approx);
    char segs[STATS_SEGS_MAX][STATS_SEG_LEN];
    int n = format_stats_segments(segs, ctx, compact_context_limit(p, model), out, cached,
                                  config_bool("stats.verbose"), elapsed_ms, spend, approx);
    if (n == 0)
        return;

    struct disp *d = &r->disp;
    render_open_block(r);
    disp_raw(ANSI_DIM);

    /* Greedy fill: emit segments joined by " · ", breaking to a fresh line
     * when the next one wouldn't fit. Segment text is ASCII (byte length ==
     * columns); only the separator's '·' is multibyte, hence the explicit
     * 3-column charge for it. */
    int width = display_width();
    int col = 0;
    for (int i = 0; i < n; i++) {
        int len = (int)strlen(segs[i]);
        if (col > 0) {
            if (col + 3 + len > width) {
                disp_putc(d, '\n');
                col = 0;
            } else {
                disp_printf(d, " · ");
                col += 3;
            }
        }
        disp_printf(d, "%s", segs[i]);
        col += len;
    }
    disp_raw(ANSI_RESET);
    disp_putc(d, '\n');
    fflush(stdout);
}

/* Per-stream side-channel hook: label/timer bookkeeping plus the
 * cancel signal, called from libcurl's progress callback (~1Hz) and on
 * every received chunk while the agent thread is parked inside
 * curl_easy_perform.
 *
 * A pause in ordinary streaming text deliberately shows nothing here:
 * the text itself is the progress indicator, and a spinner popping in
 * and out between chunks reads as flicker. Every no-visible-output
 * wait has its own spinner path elsewhere; the tick only drives the
 * two clocks that need wall time, the retry countdown and the stalled
 * table buffer. */
static int agent_stream_tick(void *user)
{
    struct render_ctx *r = user;
    /* Retry countdown wins over the other tick windows: during a retry
     * sleep the model isn't streaming at all. Repaint each tick so the
     * seconds count visibly shrinks. */
    if (r->retry_deadline_at) {
        update_retry_label(r);
    } else if (r->md && md_in_table(r->md)) {
        /* Table buffering renders nothing until the grid lays out;
         * surface "composing..." once the silence outlives the
         * threshold (a fast table finalizes first and never shows it).
         * render_text_chunk keeps it up across row deltas; this covers
         * a mid-table stall where no delta arrives to re-issue it. */
        if (!r->table_composing && r->last_text_at &&
            monotonic_ms() - r->last_text_at >= TABLE_IDLE_TIMEOUT_MS)
            render_table_spinner_show(r);
    }
    return interrupt_requested();
}

static int on_event(const struct stream_event *ev, void *user)
{
    struct event_ctx *ec = user;
    struct render_ctx *r = ec->r;
    struct disp *d = &r->disp;

    /* last_text_at tracks "time since the last byte the user could
     * see" — the tick's stall clock for the table-composing spinner.
     * Stream-ending events close the window; streamed item seams close
     * it via render_stream_seam. */
    if (ev->kind == EV_DONE || ev->kind == EV_ERROR)
        r->last_text_at = 0;
    /* Any event other than EV_RETRY itself means we're past the
     * backoff sleep — clear the countdown so per-event label updates
     * aren't fighting the tick. */
    if (ev->kind != EV_RETRY && r->retry_deadline_at) {
        r->retry_deadline_at = 0;
        spinner_set_label(r->spinner, "working", "working...");
    }

    switch (ev->kind) {
    case EV_TEXT_DELTA:
        /* Strip the first delta's leading newlines, open RS_TEXT, feed —
         * shared with resume replay so both render identically. */
        render_text_delta(r, ev->u.text_delta.text, strlen(ev->u.text_delta.text));
        break;
    case EV_TOOL_CALL_START: {
        /* Name the otherwise-anonymous args-streaming window. The
         * settle window does the smoothing: a fast call dispatches
         * before the label surfaces, and a batch shares one "compose"
         * key so per-call name swaps don't bounce through "working...".
         *
         * Naming only the latest-started call is correct for every
         * backend we target (each streams one call to completion
         * before the next); a hypothetical announce-all-then-backfill
         * order would mislabel cosmetically and self-correct at
         * dispatch — not worth an id->name map. */
        const char *name = ev->u.tool_call_start.name;
        render_stream_seam(r);
        if (name && *name) {
            char buf[64];
            snprintf(buf, sizeof(buf), "[%s] composing...", name);
            spinner_request_label(r->spinner, "compose", buf);
        }
        break;
    }
    case EV_REASONING_ITEM:
        render_stream_seam(r);
        break;
    case EV_TOOL_CALL_END:
        /* Args finalized — request neutral so only a genuine post-args
         * stall stops the label claiming the tool is still composing;
         * the normal batch cadence outruns the settle window. */
        spinner_request_label(r->spinner, "working", "working...");
        break;
    case EV_TOOL_CALL_DELTA:
        /* No live display: tool calls render as a single block during
         * dispatch so parallel calls don't visually interleave. */
        break;
    case EV_REASONING_DELTA: {
        /* Requested even when reasoning is invisible — the spinner
         * still shows, and a settled "thinking..." tells the user the
         * quiet pause is the model, not the network. */
        spinner_request_label(r->spinner, "thinking", "thinking...");
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
        /* Shared "processing" key: the settle clock runs once for the
         * whole prefill phase while each event refreshes the pending
         * percentage, so a short prefill never surfaces the label and
         * a long one ticks live after it settles. */
        spinner_request_label(r->spinner, "processing", buf);
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
    /* Active preset stance, shown dim as a qualifier of hax itself
     * ("this is hax in review trim"). Load-bearing, not decoration: a
     * preset may have swapped the system prompt, and the name is the only
     * at-a-glance signal that a stance — persona included — is in effect. */
    const char *preset = config_str("preset");
    char *stance = (preset && *preset) ? xasprintf("[%s] ", preset) : xstrdup("");
    if (!p) {
        /* No provider could be constructed (the configured/default one isn't
         * usable — e.g. codex not logged in). The REPL still starts; point
         * the user at /provider to choose a working one. */
        printf("%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM
               "%s› no provider — use /provider" ANSI_BOLD_OFF "\n",
               bar, stance);
        printf("%s " ANSI_DIM "ctrl-d quit · try /help" ANSI_BOLD_OFF "\n", bar);
        free(stance);
        return;
    }
    const char *name = p->name ? p->name : "?";
    const char *model_label = s->model_label ? s->model_label : s->model;
    if (!s->model || !*s->model)
        /* No model resolved (a provider with no default, nothing configured
         * yet). The REPL still starts; point the user at /model. */
        printf("%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM
               "%s› %s · no model — use /model" ANSI_BOLD_OFF "\n",
               bar, stance, name);
    else if (s->reasoning_effort)
        printf("%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM "%s› %s · %s · %s" ANSI_BOLD_OFF
               "\n",
               bar, stance, name, model_label, s->reasoning_effort);
    else
        printf("%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM "%s› %s · %s" ANSI_BOLD_OFF "\n",
               bar, stance, name, model_label);
    printf("%s " ANSI_DIM "ctrl-d quit · try /help" ANSI_BOLD_OFF "\n", bar);
    free(stance);
}

/* Provider name for session-log / -load metadata, tolerating a not-yet-
 * selected provider (interactive startup when the configured one couldn't
 * construct — the user picks one with /provider). */
static const char *provider_log_name(const struct provider *p)
{
    return p && p->name ? p->name : "none";
}

void agent_set_provider(struct agent_state *st, struct provider *newp)
{
    struct provider *old = (struct provider *)st->provider;
    /* The open pricing segment accumulated under the old provider's rates;
     * price it while they're still resolvable (old catalog_id + old model).
     * Inherent loss window: if the catalog hasn't landed yet, the segment's
     * tokens are dropped — unavoidable here, since after destroy() there is
     * no old provider left to price against later. */
    agent_stats_settle(st, st->sess ? st->sess->model : NULL);
    st->provider = newp;
    /* Destroy after the swap so the picker/selection code never observes a
     * half-replaced state; destroy() joins the old provider's bg probes. */
    if (old)
        old->destroy(old);
}

int agent_apply_settings(struct agent_state *st)
{
    struct agent_session *s = st->sess;
    struct provider *p = (struct provider *)st->provider;
    /* Snapshot the model before reconfigure overwrites it, to tell a real
     * /model change from a /provider or /effort apply that left it the same. */
    char *prev_model = s->model ? xstrdup(s->model) : NULL;
    if (agent_session_reconfigure(s, p) != 0) {
        free(prev_model);
        return -1;
    }

    /* A model switch can change the context window (codex/openrouter key it by
     * model), so ask the provider to re-probe for the new model — but only when
     * the model actually changed, so a bare /effort tweak doesn't trigger a
     * needless network probe (and the cancel/join of the in-flight one).
     * reconfigure already resolved s->model from the committed "model" override;
     * providers without a model-specific probe leave the hook NULL. On a
     * /provider switch the new provider's construction probe ran before the
     * model was committed, so the slug it used could be stale: when the model
     * differs we re-probe and correct it here; when it's identical that probe
     * already used the right slug, so skipping is correct too. */
    int model_changed = (prev_model == NULL) != (s->model == NULL) ||
                        (prev_model && s->model && strcmp(prev_model, s->model) != 0);
    /* A real model change can change the catalog rates the open pricing
     * segment accumulated under — settle it against the outgoing model.
     * Only now, once reconfigure succeeded and the change is confirmed: an
     * effort-only or failed apply must keep the segment open, so a
     * late-landing catalog fetch can still price it (see
     * agent_stats_settle). On a /provider switch agent_set_provider already
     * settled, leaving the segment empty and this a no-op. */
    if (model_changed)
        agent_stats_settle(st, prev_model);
    free(prev_model);
    if (model_changed && p && p->refresh_context)
        p->refresh_context(p, s->model);

    /* reconfigure rebuilt sess->sys (its env block names the new model), so
     * re-key the HAX_TRANSCRIPT mirror to it: rewrite the header and replay
     * history (like /new, but keeping the conversation) so the file keeps
     * matching the Ctrl-T view instead of claiming later turns used the old
     * system prompt. No-op when HAX_TRANSCRIPT is unset. (The session log's
     * per-item reasoning stamp tracks the switch on its own — items carry
     * their own provider+model now.) */
    transcript_log_reset(st->tlog, s->sys, s->tools, s->n_tools);
    transcript_log_append(st->tlog, s->items, s->n_items);

    /* Keep the session-log header in step with the live settings. The header
     * is written lazily on the first append, so a session that starts
     * provider-less (or with a stale startup model) and is reconfigured before
     * its first prompt records the provider/model the user actually used,
     * rather than the startup placeholder. After the header is on disk this is
     * a no-op for the current file; the next /new carries the values forward.
     * (Per-item reasoning blobs carry their own provider+model stamp, so a
     * mid-session switch stays correct independent of this header.) */
    session_log_set_meta(st->slog, provider_log_name(p), s->model, s->reasoning_effort);

    /* On an empty conversation the startup banner is usually still on
     * screen just above, boldly asserting the old settings; a dim line
     * under it would read as subordinate to that stale header. Reprint the
     * banner instead — the same clean-break signal /new gives — so the
     * loudest statement on screen is the true one (this also corrects a
     * "no provider / no model" startup banner after the pick that fixed
     * it). render_open_block supplies the leading gap the banner expects;
     * its raw printf output ends on a fresh line, so resync the trail the
     * same way the dispatcher does for raw-output handlers. */
    if (s->n_items == 0) {
        render_open_block(st->r);
        agent_print_banner(p, s);
        st->r->disp.trail = 1;
        fflush(stdout);
        return 0;
    }

    /* Mid-conversation, a dim "[switched to …]" line confirms the change
     * on screen — a banner here would falsely imply a reset. It is a
     * UI hint only — deliberately NOT appended to s->items or the logs: the
     * model can't act on a settings change, so injecting it into the
     * conversation would just be context noise (and skew the transcript /
     * --resume view away from what the model actually saw). */
    const char *model_label = s->model_label ? s->model_label : s->model;
    char *label = s->reasoning_effort
                      ? xasprintf("switched to %s · %s · %s", p->name ? p->name : "?", model_label,
                                  s->reasoning_effort)
                      : xasprintf("switched to %s · %s", p->name ? p->name : "?", model_label);

    render_open_block(st->r);
    disp_raw(ANSI_DIM);
    disp_printf(&st->r->disp, "%s", label);
    disp_raw(ANSI_RESET);
    disp_putc(&st->r->disp, '\n');
    fflush(stdout);
    free(label);
    return 0;
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
    /* A fresh conversation starts its /session ledger at zero too. */
    memset(&st->stats, 0, sizeof(st->stats));
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
                /* A compaction seed is synthetic — mark the boundary the way
                 * the live path's notice did instead of echoing the whole
                 * summary as a typed prompt. */
                if (it->compact_seed) {
                    render_open_block(r);
                    disp_raw(ANSI_DIM);
                    disp_printf(&r->disp, "── conversation compacted ──");
                    disp_raw(ANSI_RESET);
                    disp_putc(&r->disp, '\n');
                } else {
                    replay_user_echo(r, it->text);
                }
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
    if (session_load(path, &loaded, &nl, NULL) != 0 || nl == 0) {
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
    st->slog = session_log_resume(path, provider_log_name(st->provider), s->model,
                                  s->reasoning_effort, nl);
    transcript_log_reset(st->tlog, s->sys, s->tools, s->n_tools);
    transcript_log_append(st->tlog, s->items, s->n_items);

    /* Old bash temp files referenced by the prior conversation are now
     * unreachable, same as on /new. */
    bash_cleanup_tempfiles();
    replay_user_turn(st->r, s);
}

/* Event sink for the summary stream: feed the turn assembler, capture any
 * error, and account the round-trips into the session totals. Deliberately
 * renders nothing — the summary is drawn as a spinner-only operation and
 * surfaced as a one-line notice, not streamed as a normal assistant answer. */
struct compact_ev {
    struct turn *turn;
    char *err;                   /* owned; set from EV_ERROR */
    struct session_stats *stats; /* accounted into on terminal events */
};

static int compact_on_event(const struct stream_event *ev, void *user)
{
    struct compact_ev *ce = user;
    if (ev->kind == EV_ERROR && !ce->err)
        ce->err = xstrdup(ev->u.error.message ? ev->u.error.message : "stream failed");
    /* Compaction round-trips are real model requests, so their usage
     * counts into /session's totals (token sums, spend) exactly like the
     * main loop's. The *request count* is NOT tallied here: a
     * user-cancelled attempt emits no terminal event at all, so
     * agent_compact takes it from compact_summarize's attempts counter
     * instead. The window snapshot (last_ctx) is deliberately not
     * touched: the summarize request's input spans the full
     * pre-compaction history plus prompt, which would misstate the
     * post-compaction window until the next turn reports the real one. */
    if (ce->stats && ev->kind == EV_DONE) {
        const struct stream_usage *u = &ev->u.done.usage;
        if (u->input_tokens >= 0)
            ce->stats->input_tokens += u->input_tokens;
        if (u->output_tokens >= 0)
            ce->stats->output_tokens += u->output_tokens;
        if (u->cached_tokens > 0)
            ce->stats->cached_tokens += u->cached_tokens;
        spend_account(&ce->stats->spend, u);
    }
    turn_on_event(ev, ce->turn);
    return 0;
}

/* Draw a dim, out-of-band "── … ──" status line for compaction. */
static void compact_notice(struct render_ctx *r, const char *fmt, ...)
{
    va_list ap;
    char buf[256];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    render_open_block(r);
    disp_raw(ANSI_DIM);
    disp_printf(&r->disp, "── %s ──", buf);
    disp_raw(ANSI_RESET);
    disp_putc(&r->disp, '\n');
    fflush(stdout);
}

int agent_compact(struct agent_state *st, const char *instructions, int is_auto)
{
    struct agent_session *s = st->sess;
    struct provider *p = (struct provider *)st->provider;
    struct render_ctx *r = st->r;

    if (!p) {
        /* No provider to summarize with. Only reachable via a manual
         * /compact before one is picked (auto-compaction needs a streamed
         * turn, which the stream guard already blocks without a provider). */
        if (!is_auto)
            compact_notice(r, "no provider selected — use /provider");
        return 0;
    }
    if (!s->model || !*s->model) {
        /* Provider live but no model resolved (started against one with no
         * default and nothing configured). Compaction streams with s->model,
         * so guard it just like the main stream path — reachable via manual
         * /compact on a resumed session whose history is non-empty. Auto
         * never gets here: it follows a streamed turn the no-model stream
         * guard already blocks. */
        if (!is_auto)
            compact_notice(r, "no model selected — use /model (or /provider)");
        return 0;
    }
    if (s->n_items == 0) {
        if (!is_auto)
            compact_notice(r, "nothing to compact");
        return 0;
    }

    /* Close any block left open by the caller (a tool cluster on the mid-task
     * auto path, slash output on the manual path) before raising the spinner,
     * so it lands on a clean line regardless of entry point. */
    render_transition(r, RS_IDLE);

    /* Spinner-only stream — mirror the main loop's begin/stream/close, but
     * with no per-event rendering and a "compacting..." label. compact_on_event
     * feeds the turn and captures usage / the error message for the notice. */
    render_stream_begin(r);
    spinner_set_label(r->spinner, "compacting", "compacting...");

    struct turn t;
    turn_init(&t);
    struct compact_ev ce = {.turn = &t, .stats = &st->stats};

    interrupt_clear();
    interrupt_arm();
    int attempts = 0;
    char *summary = compact_summarize(s, p, instructions, &t, compact_on_event, &ce,
                                      agent_stream_tick, r, &attempts);
    /* Counted by compact_summarize, not the event sink: a cancelled
     * attempt aborts the transfer without any terminal event, but it
     * still hit the backend and /session's "requests" counts it, same
     * as the main loop's post-stream unconditional increment. */
    st->stats.requests += attempts;
    interrupt_settle();
    int cancelled = interrupt_requested();
    interrupt_disarm();

    render_transition(r, RS_IDLE);

    /* A cancel surfaces as resp.cancelled (not EV_ERROR), so compact_summarize
     * may have returned a partial summary — discard it. */
    if (cancelled) {
        free(summary);
        summary = NULL;
    }

    if (!summary || !summary[0]) {
        if (cancelled)
            compact_notice(r, "compaction cancelled");
        else if (ce.err)
            compact_notice(r, "compaction failed: %s", ce.err);
        else
            compact_notice(r, "compaction produced no summary");
        free(summary);
        free(ce.err);
        turn_reset(&t);
        return 0;
    }
    free(ce.err);
    turn_reset(&t);

    compact_apply(s, st->slog, st->tlog, summary);
    free(summary);

    compact_notice(r, "conversation compacted");
    return 1;
}

int agent_run(struct provider **provider, const struct hax_opts *opts)
{
    /* `p` is the live provider: it starts as the caller's and is replaced
     * in place by a runtime /provider switch (slash handler swaps
     * state.provider; we resync `p` from it after each dispatch). The final
     * value is written back to *provider so the caller destroys whatever is
     * live at exit. */
    struct provider *p = *provider;
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
        if (session_load(opts->resume_path, &loaded, &nl, NULL) != 0) {
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
    struct render_ctx r = {.disp = {.trail = 1}, .show_reasoning = reasoning_visible()};
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
    input_set_modal_completer(input, &file_mention_completer);
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
    state.slog = opts->resume_path
                     ? session_log_resume(opts->resume_path, provider_log_name(p), sess.model,
                                          sess.reasoning_effort, n_resumed)
                     : session_log_open(provider_log_name(p), sess.model, sess.reasoning_effort);
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
            /* A /provider switch swaps state.provider (destroying the old
             * one); resync the local so the next turn streams against the
             * new provider and its context-limit / usage reads track it. */
            p = (struct provider *)state.provider;
            input_history_add_session(input, line);
            free(line);
            continue;
        }

        input_history_add(input, line);

        /* No provider yet (the configured/default one couldn't construct, so
         * the REPL started without one): we can't stream. Checked before the
         * model guard because HAX_MODEL may be set even when the provider is
         * absent. Keep the prompt recallable and point at /provider. */
        if (!p) {
            /* Same leading blank-line gap a model turn or slash note gets:
             * emit it through disp (trail is 1 here — one newline below the
             * echoed prompt) so the note doesn't butt against the input, then
             * reset trail to model the fresh line ui_note's own newline left. */
            disp_block_separator(&r.disp);
            ui_note("no provider selected — use /provider to choose one, then resend");
            r.disp.trail = 1;
            free(line);
            continue;
        }

        /* No model yet (started against a provider with no default and
         * nothing configured): we can't stream. Keep the prompt in the
         * editor history so the user can recall it after picking, and point
         * them at the runtime selectors rather than failing the launch. */
        if (!sess.model || !*sess.model) {
            disp_block_separator(&r.disp); /* leading gap, as above */
            ui_note("no model selected — use /model (or /provider) to choose one, then resend");
            r.disp.trail = 1;
            free(line);
            continue;
        }

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

        /* A user turn on a catalog-mapped provider is about to need catalog
         * metadata (cost estimate, window fallback) — kick the background
         * snapshot refresh so it can land while the model generates. One
         * attempt per run; a no-op every call after. A snapshot that has
         * been failing to refresh for weeks is the one condition worth a
         * line here: estimates may have drifted. */
        if (p->catalog_id) {
            long stale_days = catalog_prefetch();
            if (stale_days > 0) {
                disp_block_separator(&r.disp);
                ui_note("model catalog last refreshed %ld days ago — cost estimates may be stale",
                        stale_days);
                r.disp.trail = 1;
            }
        }

        /* Aggregated across every model call this user turn produces.
         * ctx and cached track the latest reported value (= current window
         * state, since each call's input subsumes the prior call's prefix);
         * out is a running sum so the summary reflects total tokens
         * generated in response to this prompt. -1 means "no call reported
         * this number yet". */
        long user_turn_ctx = -1, user_turn_out = -1, user_turn_cached = -1;
        long user_turn_start_ms = monotonic_ms();
        int user_turn_errored = 0;
        int user_turn_interrupted = 0;

        /* Immediate label reset: a fresh user turn is a clean slate,
         * and the previous turn's promoted label must not describe it
         * for a settle window (no flicker risk — the spinner was
         * hidden while the user typed). The timer uses the same clock
         * as the end-of-turn stats line so the two agree. */
        spinner_set_label(r.spinner, "working", "working...");
        spinner_set_timer(r.spinner, user_turn_start_ms);

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
            struct event_ctx ec = {.r = &r, .turn = &t, .usage = {-1, -1, -1, -1, -1}};
            /* agent_stream_tick combines cancel (Esc) with idle
             * detection: ~1Hz from libcurl's progress callback inside
             * curl_easy_perform, plus on every received chunk. */
            p->stream(p, &ctx, sess.model, on_event, &ec, agent_stream_tick, &r);
            /* Every stream call is one model round-trip, /session's
             * "requests" — counted regardless of outcome (an errored or
             * interrupted attempt still hit the backend). */
            state.stats.requests++;

            if (t.error) {
                /* Mid-stream error (network drop, "length"/"content_filter"
                 * truncation, provider parse failure, ...). Preserve the
                 * partial assistant text in conversation history so the
                 * user can ask the model to "continue" on the next turn
                 * without losing what was already streamed. The on-screen
                 * "[error: ...]" line was already drawn by on_event;
                 * absorb_aborted_turn shapes what lands in history. */
                struct absorb_outcome o = absorb_aborted_turn(&sess, &t, &state.stats);
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

            /* Session totals (/session): sums of what this round-trip
             * reported. Unlike the display trio above these are plain
             * accumulators — unreported fields just don't contribute. */
            if (user_turn_ctx >= 0) {
                state.stats.last_ctx = user_turn_ctx;
                state.stats.last_limit = compact_context_limit(p, sess.model);
            }
            if (ec.usage.input_tokens >= 0)
                state.stats.input_tokens += ec.usage.input_tokens;
            if (ec.usage.output_tokens >= 0)
                state.stats.output_tokens += ec.usage.output_tokens;
            if (ec.usage.cached_tokens > 0)
                state.stats.cached_tokens += ec.usage.cached_tokens;
            spend_account(&state.stats.spend, &ec.usage);

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
                struct absorb_outcome o = absorb_aborted_turn(&sess, &t, &state.stats);
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
             * one block per call so parallel calls don't interleave.
             * Esc partway through the batch flips remaining calls to a
             * synthesized "[interrupted]" result so the conversation
             * stays well-formed; settle first so a fast-returning tool
             * doesn't race past a pending \x1b in the classifier. */
            size_t current_end = sess.n_items;
            for (size_t i = n_before; i < current_end; i++) {
                if (sess.items[i].kind != ITEM_TOOL_CALL)
                    continue;
                stats_count_tool_call(&state.stats, sess.items[i].tool_name);
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
                append_interrupt_marker(&sess);
                user_turn_interrupted = 1;
                break;
            }

            /* Mid-task auto-compaction: the model called tools and we're about
             * to loop back for another round-trip. If this round-trip's
             * reported context usage already nears the window, summarize now —
             * before the next request — so a long autonomous tool chain can't
             * run itself into an overflow. The just-completed work (assistant
             * text, tool calls, results) is summarized and the model continues
             * from the seed on the next iteration. Distinct from the
             * end-of-user-turn check below, which fires before the *next user
             * message* is added so that message is preserved verbatim. */
            if (compact_should_auto(user_turn_ctx, compact_context_limit(p, sess.model))) {
                agent_compact(&state, NULL, 1);
                /* Esc during auto-compaction is the user's stop signal:
                 * agent_compact aborts the summary but leaves the flag latched.
                 * Honor it like any mid-turn Esc — stop the tool loop rather
                 * than clearing it and firing another round-trip against the
                 * (still uncompacted) history. */
                if (interrupt_requested()) {
                    append_interrupt_marker(&sess);
                    user_turn_interrupted = 1;
                    break;
                }
                /* agent_compact runs its own arm/disarm and leaves the watcher
                 * disarmed; the inner loop relies on it staying armed, so
                 * re-arm (idempotent) for the remaining round-trips. */
                interrupt_clear();
                interrupt_arm();
            }
        }
        interrupt_disarm();
        keepawake_release();
        /* The user turn is over: the end-of-user-turn auto-compaction
         * below and the next user turn's pre-stream spinner must not
         * carry this one's counter. */
        spinner_set_timer(r.spinner, 0);

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

        /* Time worked counts errored/interrupted turns too — the wall time
         * was spent either way, and /session's total should reflect it. */
        long user_turn_ms = monotonic_ms() - user_turn_start_ms;
        state.stats.worked_ms += user_turn_ms;
        state.stats.turns++;

        if (!user_turn_errored)
            display_stats_line(&r, p, sess.model, user_turn_ctx, user_turn_out, user_turn_cached,
                               user_turn_ms, &state.stats);

        /* Auto-compaction: once the reported context usage nears the window,
         * summarize and replace history before the next prompt. Skipped on an
         * errored/interrupted user turn (no reliable token count, and the
         * user may want to retry against intact history). Runs at this natural
         * pause — the model has finished responding and we're about to wait
         * for input — so no mid-task continuation is needed. */
        if (!user_turn_errored && !user_turn_interrupted &&
            compact_should_auto(user_turn_ctx, compact_context_limit(p, sess.model)))
            agent_compact(&state, NULL, 1);

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

    /* Hand the live provider back so the caller destroys the one that's
     * current at exit, not the one it passed in (a /provider switch may
     * have replaced it). */
    *provider = p;

    spinner_free(r.spinner);
    input_free(input);
    if (r.md)
        md_free(r.md);
    transcript_log_close(tlog);
    session_log_close(state.slog);
    agent_session_free(&sess);
    return 0;
}
