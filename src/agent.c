/* SPDX-License-Identifier: MIT */
#include "agent.h"

#include <jansson.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "slash.h"
#include "tool.h"
#include "transcript.h"
#include "turn.h"
#include "util.h"
#include "render/ctrl_strip.h"
#include "render/disp.h"
#include "render/markdown.h"
#include "render/spinner.h"
#include "render/tool_render.h"
#include "system/spawn.h"
#include "terminal/ansi.h"
#include "terminal/input.h"
#include "terminal/interrupt.h"
#include "terminal/notify.h"

#define INTERRUPT_MARKER "[interrupted]"

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
     * never reached EV_TOOL_CALL_END. */
    int had_state = t->in_text || t->n_pending > 0 || t->n_items > 0;
    int had_partial_text = t->in_text;
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

/* What visual mode is currently "open" on the terminal — each state
 * owns specific bytes (SGR runs, cursor position, in-flight spinner
 * variant) that must be unwound before drawing anything else.
 *
 *   RS_IDLE      Nothing open: cursor at column 0, no SGR active.
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

/* Per-stream slice the event callback needs alongside render_ctx. */
struct event_ctx {
    struct render_ctx *r;
    struct turn *turn;
    /* Filled in from EV_DONE; -1/-1/-1 if the provider didn't report. */
    struct stream_usage usage;
};

/* Clear the idle inline overlay (SPINNER_INLINE_TEXT) if it's up. The
 * overlay is a "still alive" indicator drawn on top of an in-flight
 * content stream; any new render activity supersedes it. Called from
 * render_transition (covering both cross-state and same-state cases)
 * so handlers don't need to repeat it after every transition call. */
static void render_clear_idle(struct render_ctx *r)
{
    if (r->idle_shown) {
        spinner_hide(r->spinner);
        r->idle_shown = 0;
    }
}

/* Move the live render state to `to`: close whatever is currently
 * open (SGR reset, md tail flush, state-owned spinner hidden), then
 * open the target (block separator, fresh SGR, canonical label).
 *
 * Spinner ordering invariant: non-cluster spinners are hidden before
 * the open-half's block separator advances the cursor past the glyph
 * (which would make \b-space-\b erase impossible). The cluster's own
 * spinner is hidden inside the close-half of RS_CLUSTER where
 * was_inline drives correct NL/trail accounting. */
static void render_transition(struct render_ctx *r, enum render_state to)
{
    render_clear_idle(r);
    if (r->state == to)
        return;

    switch (r->state) {
    case RS_IDLE:
        break;
    case RS_REASONING:
        if (r->md)
            md_flush(r->md);
        disp_raw(ANSI_RESET);
        disp_putc(&r->disp, '\n');
        break;
    case RS_TEXT:
        if (r->md)
            md_flush(r->md);
        break;
    case RS_CLUSTER: {
        int was_inline = spinner_hide(r->spinner);
        if (was_inline)
            disp_putc(&r->disp, '\n');
        else
            r->disp.trail = 1;
        fflush(stdout);
        r->cluster_last_tool = NULL;
        r->cluster_line_used = 0;
        break;
    }
    }

    switch (to) {
    case RS_IDLE:
        /* No separator emitted — out-of-band block writers
         * (render_open_block, display_tool_header) own that decision. */
        break;
    case RS_REASONING:
        spinner_hide(r->spinner);
        disp_block_separator(&r->disp);
        disp_raw(ANSI_DIM ANSI_ITALIC);
        spinner_set_label(r->spinner, "thinking...");
        /* Suppress md's inline SGR so the dim+italic span above is
         * the only active style; wrap and block structure still run. */
        if (r->md)
            md_set_styled(r->md, 0);
        break;
    case RS_TEXT:
        spinner_hide(r->spinner);
        disp_block_separator(&r->disp);
        spinner_set_label(r->spinner, "working...");
        r->disp.saw_text = 1;
        if (r->md)
            md_set_styled(r->md, 1);
        break;
    case RS_CLUSTER:
        spinner_hide(r->spinner);
        disp_block_separator(&r->disp);
        break;
    }

    r->state = to;
}

/* Commit any pending output to the terminal without changing state:
 * clear the idle overlay, flush md's lookahead tail if we're in a
 * content stream, and reset the spinner label to the neutral default.
 * Called at mid-stream content-block seams (text/reasoning → tool
 * call, or one reasoning item → the next) so the trailing fragment of
 * the prior block lands before the next block's bytes arrive. State
 * is left untouched so any open SGR (RS_REASONING's dim+italic)
 * persists across the seam. */
static void render_flush(struct render_ctx *r)
{
    render_clear_idle(r);
    if (r->md && (r->state == RS_TEXT || r->state == RS_REASONING))
        md_flush(r->md);
    spinner_set_label(r->spinner, "working...");
}

/* Pre-stream housekeeping: arm fresh idle/retry bookkeeping for the
 * new stream, then show the "working..." spinner — unless we're
 * continuing a cluster, whose own end-of-line spinner is the alive
 * indicator across the gap. */
static void render_stream_begin(struct render_ctx *r)
{
    r->last_text_at = 0;
    r->idle_shown = 0;
    r->retry_deadline_at = 0;
    if (r->state == RS_CLUSTER)
        return;
    disp_block_separator(&r->disp);
    spinner_set_label(r->spinner, "working...");
    spinner_show(r->spinner);
}

/* Post-stream housekeeping: hide the pre-stream spinner unless a
 * cluster is open. Cluster's spinner spans the gap to the next call
 * (verbose tool dispatch or a continuation stream). */
static void render_stream_end(struct render_ctx *r)
{
    if (r->state == RS_CLUSTER)
        return;
    spinner_hide(r->spinner);
}

/* Return to a clean idle line and open a fresh visual block — for
 * out-of-band rendering (error line, usage stats, interrupt marker)
 * that should land separated from prior content. */
static void render_open_block(struct render_ctx *r)
{
    render_transition(r, RS_IDLE);
    /* RS_IDLE → RS_IDLE is a no-op transition, so the "working..." line
     * spinner from render_stream_begin can still be occupying the
     * current row when we get here (e.g. EV_ERROR fired before any
     * SSE bytes, or EV_DONE on an empty assistant turn). The spinner's
     * \r-prefixed redraw doesn't update disp's column tracking, so the
     * out-of-band block we're about to write would land on the same
     * row as the spinner's glyph+label — and then the post-stream
     * spinner_hide's \r + erase-line would wipe the row, taking the
     * error/usage text with it. Hide explicitly here so the line is
     * cleared before we write to it; no-op when the spinner is already
     * off (the common path after any RS_TEXT/RS_REASONING run). */
    spinner_hide(r->spinner);
    disp_block_separator(&r->disp);
}

/* Write a chunk of model-produced text into the currently-open content
 * stream (RS_TEXT or RS_REASONING). Routes through md when enabled,
 * falls through to disp otherwise; arms idle detection so the tick
 * can surface the inline spinner if the model goes quiet. */
static void render_text_chunk(struct render_ctx *r, const char *s, size_t n)
{
    if (r->md)
        md_feed(r->md, s, n);
    else
        disp_write(&r->disp, s, n);
    fflush(stdout);
    r->last_text_at = monotonic_ms();
}

/* Paint the spinner with the retry countdown label. Called on EV_RETRY
 * (so the user sees the new state immediately) and from the stream tick
 * (so the seconds count visibly decreases during the backoff sleep).
 * spinner_set_label gates on strcmp internally, so calling this every
 * tick is cheap until the second changes. */
static void update_retry_label(struct render_ctx *r)
{
    long remaining = r->retry_deadline_at - monotonic_ms();
    /* Round up so the countdown shows "1s" through the final second
     * instead of dropping to "0s" with time still on the clock. Floor
     * at 1s when we're inside the last 100ms — saying "in 0s" reads as
     * "stuck", and the actual flip to a fresh attempt is imminent. */
    long secs = (remaining + 999) / 1000;
    if (secs < 1)
        secs = 1;
    char buf[64];
    snprintf(buf, sizeof(buf), "retrying in %lds (attempt %d/%d)...", secs, r->retry_attempt,
             r->retry_max);
    spinner_set_label(r->spinner, buf);
}

/* How long after the last text byte we wait before surfacing the
 * "still alive" inline glyph. Shorter than the silent-cluster's
 * INLINE_TIMEOUT_MS because the inline glyph is non-disruptive — one
 * dim cell at the cursor's natural position, no paragraph break, no
 * disp bookkeeping perturbed — so we can show it sooner without the
 * risk of flicker between rapid deltas. */
#define TEXT_IDLE_TIMEOUT_MS 1500

/* Cells reserved at the right edge of a quiet line: the trailing space
 * before the spinner glyph (1) + the glyph itself (1) + breathing room
 * so the spinner doesn't crowd the right margin (~6). Pulled out here
 * so the silent-header sizing and coalesce-overflow check stay in
 * sync. */
#define QUIET_LINE_MARGIN 8

/* Return a pointer into `path` at the basename — last component after
 * the final '/'. Trailing slashes (`src/`) fall back to the full path
 * since the basename would be empty. Returns "?" for NULL/empty. */
static const char *basename_view(const char *path)
{
    if (!path || !*path)
        return "?";
    const char *slash = strrchr(path, '/');
    if (!slash || slash[1] == '\0')
        return path;
    return slash + 1;
}

/* Decide whether this call should render with the silent-preview flow.
 * Static `silent_preview` flag wins when set; otherwise the optional
 * `is_silent` callback gets to inspect args (used by bash to classify
 * exploration commands at runtime). */
static int call_is_silent(const struct tool *t, const struct item *call)
{
    if (!t)
        return 0;
    if (t->silent_preview)
        return 1;
    if (t->is_silent)
        return t->is_silent(call->tool_arguments_json);
    return 0;
}

/* Hard cap on the dim suffix appended after the bold display_arg
 * (read's ":N-M" line range, etc.). The model controls the suffix
 * content via tool args; without a cap, an adversarial offset/limit
 * pair could produce a suffix that dominates the row. Real suffixes
 * are well under this. */
#define HEADER_EXTRA_CAP 20

/* Verbose tool-call header: block separator, `[name]` tag, the tool's
 * display_arg (full path / command), optional dim suffix. The arg is
 * word-wrapped across up to `tool->header_rows` rows (default 1) and
 * truncated with "..." beyond that, so it can't overflow into
 * terminal-driven mid-word wrapping. Terminated with `\n` and
 * committed so the spinner or output draws below. */
static void display_tool_header(struct disp *d, const struct item *call)
{
    const struct tool *tool = find_tool(call->tool_name);
    const char *display_arg = NULL;
    json_t *root = NULL;
    if (tool && tool->def.display_arg && call->tool_arguments_json) {
        json_error_t jerr;
        root = json_loads(call->tool_arguments_json, 0, &jerr);
        if (root)
            display_arg = json_string_value(json_object_get(root, tool->def.display_arg));
    }

    disp_block_separator(d);
    disp_raw(ANSI_CYAN);
    disp_printf(d, "[%s]", call->tool_name);
    disp_raw(ANSI_RESET);

    int width = display_width();
    /* Tool names are ASCII so strlen == cells. */
    int prefix_cost = (int)strlen(call->tool_name) + 3; /* "[name] " */

    if (display_arg) {
        /* Reserve the dim extra suffix's cells out of the last row's
         * budget so the suffix stays attached without pushing the arg
         * over. format_display_extra is ASCII (e.g. read's ":30-50")
         * so byte length approximates cells. The model controls the
         * suffix content via tool args (a huge offset/limit pair on
         * read produces a 20+ char `:N-M`), so cap it here — without
         * a cap, a runaway suffix would dominate the row and shrink
         * the arg to "..." even when the arg is the user-interesting
         * part. */
        int extra_cost = 0;
        char *extra = NULL;
        if (tool && tool->format_display_extra) {
            extra = tool->format_display_extra(call->tool_arguments_json);
            if (extra && *extra) {
                int extra_cap = HEADER_EXTRA_CAP;
                if (extra_cap > width - 8)
                    extra_cap = width - 8;
                if (extra_cap < 4)
                    extra_cap = 4;
                if ((int)strlen(extra) > extra_cap) {
                    char *trimmed = truncate_for_display(extra, (size_t)extra_cap);
                    free(extra);
                    extra = trimmed;
                }
                extra_cost = (int)strlen(extra);
            }
        }

        int rows = tool && tool->header_rows > 0 ? tool->header_rows : 1;
        int first_row = width - prefix_cost;
        int mid_row = width;
        if (first_row < 8)
            first_row = 8;
        if (mid_row < 8)
            mid_row = 8;

        disp_putc(d, ' ');
        disp_raw(ANSI_BOLD);
        char *flat = flatten_for_display(display_arg);
        char *laid = reflow_for_display(flat, first_row, mid_row, rows, extra_cost);
        free(flat);
        disp_write(d, laid, strlen(laid));
        free(laid);
        disp_raw(ANSI_RESET);
        if (extra && *extra) {
            disp_raw(ANSI_DIM);
            disp_write(d, extra, strlen(extra));
            disp_raw(ANSI_RESET);
        }
        free(extra);
    } else if (call->tool_arguments_json && *call->tool_arguments_json) {
        disp_putc(d, ' ');
        disp_raw(ANSI_DIM);
        char *flat = flatten_for_display(call->tool_arguments_json);
        /* Generic JSON arg (for tools without a display_arg field): one
         * row, plain truncate. These are debug-grade — no point laying
         * out raw JSON across multiple rows. */
        int budget = width - prefix_cost;
        if (budget < 8)
            budget = 8;
        char *trimmed = truncate_for_display(flat, (size_t)budget);
        disp_write(d, trimmed, strlen(trimmed));
        free(trimmed);
        free(flat);
        disp_raw(ANSI_RESET);
    }
    disp_putc(d, '\n');
    /* Commit the trailing newline so the cursor is at column 0 of the
     * next line. The spinner shown during tool execution, or the tool
     * output itself, draws there instead of overwriting the header. */
    disp_emit_held(d);

    if (root)
        json_decref(root);
    fflush(stdout);
}

/* Silent-header writer for the start of a quiet line. For `read`,
 * `arg_text` is the file's basename (possibly with a `:N-M` slice
 * suffix); for quiet `bash`, it's the truncated command. The header is
 * NOT terminated with a newline — the caller decides whether to keep
 * the cursor parked at end-of-line for an inline spinner (reads, where
 * the next call may coalesce as `, baz.c`) or to emit `\n` and show a
 * labeled line-mode spinner below (non-coalescing kinds like bash).
 *
 * The whole header is dim — quiet calls are exploration breadcrumbs
 * that should recede visually, not compete with verbose tool blocks
 * (whose preview body is the focus) or model text. The cyan brackets
 * keep the tag scannable as a tool boundary even at lowered intensity.
 *
 * `reserve_spinner_space` adds a trailing space as breathing room for
 * the inline glyph; pass 0 when the caller will follow with `\n` so
 * redirected logs don't grow dangling spaces.
 *
 * Returns the visual byte cost of the line so far so the caller can
 * track when a coalesced line is about to overflow the terminal. */
static int write_silent_header(struct disp *d, const struct item *call, const char *arg_text,
                               int reserve_spinner_space)
{
    /* Visual budget tracking: we count what's *visible* — ANSI escapes
     * and the trailing space don't move the cursor visually, but the
     * tag, space, and arg do. Approximation; off-by-a-few is fine. */
    int used = 0;
    disp_raw(ANSI_DIM ANSI_CYAN);
    disp_printf(d, "[%s]", call->tool_name);
    /* Switch back to default foreground but keep DIM in effect so the
     * arg is dim too. ANSI_RESET would drop the dim attribute. */
    disp_raw(ANSI_FG_DEFAULT);
    used += 2 + (int)strlen(call->tool_name); /* "[name]" */
    disp_putc(d, ' ');
    used += 1;
    if (arg_text && *arg_text) {
        disp_write(d, arg_text, strlen(arg_text));
        used += (int)strlen(arg_text);
    }
    disp_raw(ANSI_RESET);
    /* Inline-spinner room: we need a space for the glyph, but only on
     * a TTY (no spinner otherwise — see write_silent_append's matching
     * tty check for why a non-TTY space would be a stray byte). */
    if (reserve_spinner_space && isatty(fileno(stdout))) {
        disp_putc(d, ' ');
        used += 1;
    }
    disp_emit_held(d); /* commit any held space so spinner lands inline */
    fflush(stdout);
    return used;
}

/* Silent-header append for read coalescing: writes ", basename" inline
 * onto the current line. Caller has already hidden the inline spinner
 * (which restored the cursor to the position right after the prior
 * filename's trailing space — disp_putc' space stays committed). */
static int write_silent_append(struct disp *d, const char *short_name)
{
    /* TTY: step back over the trailing space we left for the spinner
     * glyph, write ", short_name", then re-add the trailing space.
     * Cursor was at "...foo.c |" (| = cursor), after this we're at
     * "...foo.c, bar.c |". Backspace is safe since the line is
     * width-capped and we never wrap.
     *
     * Non-TTY: write_silent_header skipped the trailing space, so
     * there's nothing to step over — emitting `\b` would land a literal
     * control byte in the redirected log. Just append ", short_name". */
    int tty = isatty(fileno(stdout));
    if (tty)
        fputs("\b", stdout);
    disp_raw(ANSI_DIM);
    disp_write(d, ", ", 2);
    disp_write(d, short_name, strlen(short_name));
    disp_raw(ANSI_RESET);
    if (tty)
        disp_putc(d, ' ');
    disp_emit_held(d);
    fflush(stdout);
    /* Visible delta: ", " + name (+ restored space on TTY). The
     * backspace only undoes the space we'd previously laid down, which
     * we re-add at the end, so net is +2+strlen(name) regardless. */
    return 2 + (int)strlen(short_name);
}

/* Compute the arg text shown after the bracketed tag for a silent
 * call. Read uses basename of the file plus optional `:N-M`; bash uses
 * the command, truncated to fit in the available column budget.
 * `tag_cost` is the bytes consumed by `[name] ` so we know how many
 * columns are left for the arg. Returns malloc'd; caller frees. */
static char *make_silent_arg(const struct tool *tool, const struct item *call, int tag_cost,
                             int term_w)
{
    const char *name = call->tool_name;
    int budget = term_w - tag_cost - QUIET_LINE_MARGIN;
    if (budget < 8)
        budget = 8;

    if (strcmp(name, "read") == 0) {
        const char *path = NULL;
        json_t *root = NULL;
        if (call->tool_arguments_json) {
            json_error_t jerr;
            root = json_loads(call->tool_arguments_json, 0, &jerr);
            if (root)
                path = json_string_value(json_object_get(root, "path"));
        }
        const char *base = basename_view(path);
        char *extra = NULL;
        if (tool && tool->format_display_extra)
            extra = tool->format_display_extra(call->tool_arguments_json);
        char *full = (extra && *extra) ? xasprintf("%s%s", base, extra) : xstrdup(base);
        free(extra);
        if (root)
            json_decref(root);
        /* Flatten before truncating: a model could send a path with
         * embedded newlines/control bytes; basename_view's split point
         * is the last `/` so any newline elsewhere comes through and
         * would break the silent header's single-line invariant. */
        char *flat = flatten_for_display(full);
        free(full);
        char *trimmed = truncate_for_display(flat, (size_t)budget);
        free(flat);
        return trimmed;
    }
    if (strcmp(name, "bash") == 0) {
        const char *cmd = NULL;
        json_t *root = NULL;
        if (call->tool_arguments_json) {
            json_error_t jerr;
            root = json_loads(call->tool_arguments_json, 0, &jerr);
            if (root)
                cmd = json_string_value(json_object_get(root, "command"));
        }
        /* Flatten before truncating: a multi-line command (bash_classify
         * accepts e.g. `ls\npwd` as exploration) would otherwise wrap
         * the header across rows and put the inline spinner on the
         * wrong line. */
        char *flat = flatten_for_display(cmd ? cmd : "");
        char *trimmed = truncate_for_display(flat, (size_t)budget);
        free(flat);
        if (root)
            json_decref(root);
        return trimmed;
    }
    /* Generic fallback (no other tool currently goes silent). */
    return xstrdup("");
}

/* Render a synthesized "[interrupted]" block in place of running a tool,
 * and produce the matching tool_result item so the conversation stays
 * well-formed when Esc fires partway through a batch. */
static struct item dispatch_tool_skipped(struct render_ctx *r, const struct item *call)
{
    struct disp *d = &r->disp;
    display_tool_header(d, call);
    disp_tool_strip_solo(d);
    disp_raw(ANSI_DIM);
    disp_printf(d, "%s", INTERRUPT_MARKER);
    disp_raw(ANSI_RESET);
    disp_putc(d, '\n');
    fflush(stdout);
    return (struct item){
        .kind = ITEM_TOOL_RESULT,
        .call_id = xstrdup(call->call_id),
        .output = xstrdup(INTERRUPT_MARKER),
    };
}

/* Refuse a tool call without running it. Used in --raw mode: we
 * advertised no tools, so a tool_call from the provider is either a
 * model bug or a misbehaving backend — either way we MUST NOT execute
 * it. Display a refusal header for user visibility and feed an error
 * tool_result back so the conversation stays well-formed and the model
 * can recover (e.g. answer in plain text instead). */
static struct item dispatch_tool_refused(struct render_ctx *r, const struct item *call)
{
    struct disp *d = &r->disp;
    display_tool_header(d, call);
    disp_tool_strip_solo(d);
    disp_raw(ANSI_DIM);
    disp_printf(d, "[refused: --raw, no tools advertised]");
    disp_raw(ANSI_RESET);
    disp_putc(d, '\n');
    fflush(stdout);
    return (struct item){
        .kind = ITEM_TOOL_RESULT,
        .call_id = xstrdup(call->call_id),
        .output = xstrdup("error: tool calls are disabled in this session"),
    };
}

/* Silent dispatch: header-only, inline spinner, no preview. Coalesces
 * with the previous quiet line when the prior tool was the same kind
 * (currently only `read` chains visually). Output is captured for
 * conversation history exactly like the verbose path; only the live
 * display is suppressed.
 *
 * Caller (dispatch_tool_call) has already called render_transition
 * to RS_CLUSTER, so r->state is RS_CLUSTER and — on first entry —
 * a clean column-0 row is below whatever came before.
 * cluster_last_tool == NULL distinguishes "just entered the cluster"
 * from "continuing a cluster started by a previous call". */
static struct item dispatch_tool_call_silent(struct render_ctx *r, const struct item *call,
                                             const struct tool *t)
{
    struct disp *d = &r->disp;
    struct spinner *sp = r->spinner;
    int term_w = display_width();

    /* Hide the cluster's own spinner and learn what mode it was in.
     * was_inline=1: cursor sits at the end of the prior cluster line
     * (we can append/coalesce). was_inline=0: prior spinner was line
     * mode (auto-transitioned, or labeled bash) OR there's no prior
     * spinner because this is the first call of a freshly-entered
     * cluster — cursor is at column 0 of a fresh row, and coalescing
     * isn't possible from there. */
    int was_inline = spinner_hide(sp);
    int can_coalesce = was_inline && r->cluster_last_tool &&
                       strcmp(r->cluster_last_tool, "read") == 0 &&
                       strcmp(call->tool_name, "read") == 0;
    /* Reads coalesce, so the inline glyph is parked at end-of-line as
     * an attachment point for a follow-up `, baz.c`. Bash never
     * coalesces — every silent bash gets its own header line — so go
     * straight to the labeled line-mode spinner below the header
     * instead of waiting for INLINE_TIMEOUT_MS to auto-transition.
     * Surfaces the "running..." label immediately for slow commands. */
    int inline_spinner = strcmp(call->tool_name, "read") == 0;

    if (can_coalesce) {
        const char *path = NULL;
        json_t *root = NULL;
        if (call->tool_arguments_json) {
            json_error_t jerr;
            root = json_loads(call->tool_arguments_json, 0, &jerr);
            if (root)
                path = json_string_value(json_object_get(root, "path"));
        }
        const char *base = basename_view(path);
        char *extra = NULL;
        if (t && t->format_display_extra)
            extra = t->format_display_extra(call->tool_arguments_json);
        char *full = (extra && *extra) ? xasprintf("%s%s", base, extra) : xstrdup(base);
        free(extra);
        /* Flatten before measuring/appending so an embedded newline
         * in the path doesn't break the coalesced single-line header
         * (same reason as make_silent_arg's read branch). */
        char *append = flatten_for_display(full);
        free(full);
        size_t append_len = strlen(append);
        /* Cap coalesced line at ~term width so we never wrap. The
         * extra 2 covers ", " on top of the standard end-of-line
         * margin (trailing space + spinner + breathing room). */
        if (r->cluster_line_used + (int)append_len + 2 + QUIET_LINE_MARGIN > term_w) {
            /* Overflow → close current line, start a new `[read] …` header. */
            disp_putc(d, '\n');
            disp_emit_held(d);
            char *arg = make_silent_arg(t, call, 7 /* "[read] " */, term_w);
            int used = write_silent_header(d, call, arg, 1 /* coalesce path is always read */);
            free(arg);
            r->cluster_line_used = used;
        } else {
            r->cluster_line_used += write_silent_append(d, append);
        }
        free(append);
        if (root)
            json_decref(root);
    } else {
        if (r->cluster_last_tool) {
            /* Already inside the cluster but can't coalesce (different
             * silent kind, or coalesce-overflow): close the prior line
             * and write a fresh header below — but stay in RS_CLUSTER,
             * so no block separator between the lines. */
            if (was_inline) {
                disp_putc(d, '\n');
                disp_emit_held(d);
            } else {
                /* Prior spinner had auto-transitioned to line mode.
                 * spinner_hide cleared the row; cursor is at column 0
                 * of that fresh row. Sync trail so later separators
                 * compute correctly. */
                d->trail = 1;
            }
        }
        /* Else: first call after entering RS_CLUSTER — the transition
         * already left a clean column-0 row below prior content. */
        int tag_cost = 2 + (int)strlen(call->tool_name) + 1; /* "[name] " */
        char *arg = make_silent_arg(t, call, tag_cost, term_w);
        int used = write_silent_header(d, call, arg, inline_spinner);
        free(arg);
        r->cluster_line_used = used;
        if (!inline_spinner) {
            /* Close the header line so the line-mode spinner draws on
             * its own row below. Without this, the spinner thread's
             * `\r` + erase would clobber the [bash] header. */
            disp_putc(d, '\n');
            disp_emit_held(d);
        }
    }

    /* Bracket the run with "running..." / "working..." labels so the
     * spinner accurately reflects whether the tool is actively
     * executing. Silent path leaves the spinner visible after run()
     * to span the gap to the next call or stream event — without the
     * post-run reset, a stale "running..." would linger through that
     * gap (line-mode bash) or leak through an auto-transition (inline
     * read past INLINE_TIMEOUT_MS). The pre-run set ensures even
     * inline reads, if they auto-transition, surface the right label.
     * Set before show so the first frame draws with the new label
     * rather than the prior turn's residue. */
    spinner_set_label(sp, "running...");
    if (inline_spinner) {
        spinner_show_inline_header(sp);
    } else {
        spinner_show(sp);
    }

    /* Run the tool with no display callback — silent path discards live
     * stream and only keeps the canonical history. */
    char *ret = t->run(call->tool_arguments_json, NULL, NULL);
    spinner_set_label(sp, "working...");

    r->cluster_last_tool = t->def.name;

    char *history = ctrl_strip_dup(ret ? ret : "");
    free(ret);

    return (struct item){
        .kind = ITEM_TOOL_RESULT,
        .call_id = xstrdup(call->call_id),
        .output = history,
    };
}

/* Run one tool call: render the header, drive the renderer over either
 * streamed emit_display chunks or the canonical return value, and produce
 * the tool_result item that goes back to the model. The canonical history
 * is ctrl_stripped at this boundary so all tools' outputs land in the
 * conversation in the same normalized form; anything pushed through
 * emit_display is display-only and does not enter history. */
static struct item dispatch_tool_call_verbose(struct render_ctx *r, const struct item *call)
{
    struct disp *d = &r->disp;
    struct spinner *sp = r->spinner;
    display_tool_header(d, call);
    /* "⠋ running..." — the spinner re-emerges between streamed chunks
     * (and for non-streaming tools, after the single returned payload),
     * so the user always sees "running" while a tool is executing. */
    spinner_set_label(sp, "running...");
    spinner_show(sp);

    const struct tool *t = find_tool(call->tool_name);
    /* Initial mode comes straight from tool capability. For diff-capable
     * tools (write/edit) we leave R_DIFF for after run() — they never
     * stream, so we'll have the full return string in hand to check the
     * `--- ` prefix before deciding diff vs error preview. */
    enum render_mode mode = (t && t->preview_tail) ? R_HEAD_TAIL : R_HEAD_ONLY;
    struct tool_render rr;
    tool_render_init(&rr, d, sp, mode);
    char *ret = t ? t->run(call->tool_arguments_json, tool_render_emit, &rr)
                  : xasprintf("unknown tool: %s", call->tool_name);

    /* If the tool called emit_display at any point, the live preview is
     * already rendered. Otherwise feed the canonical return value through
     * once so the renderer treats both kinds uniformly. */
    if (ret && !rr.emit_called) {
        /* Empty output from a diff-capable tool means the write/edit
         * was a no-op (byte-identical content, see fs_write_with_diff).
         * Render the marker inline — feeding "" through the preview
         * renderer would leave the user staring at a bare tool header. */
        if (t && t->output_is_diff && !*ret) {
            spinner_hide(sp);
            disp_tool_strip_solo(d);
            disp_raw(ANSI_DIM);
            disp_printf(d, "(no changes)");
            disp_raw(ANSI_RESET);
            disp_putc(d, '\n');
            fflush(stdout);
        } else {
            /* Diff-capable tools' success output starts with `--- `;
             * their failure output (error messages) doesn't. Switching
             * mode here keeps a botched write/edit flowing through the
             * standard preview path instead of mis-coloring it as a diff. */
            if (t && t->output_is_diff && strncmp(ret, "--- ", 4) == 0)
                rr.mode = R_DIFF;
            tool_render_feed(&rr, ret, strlen(ret));
        }
    }
    tool_render_finalize(&rr);
    /* Spinner may still be up (no-output case, or head-full resume that
     * we never hid in finalize). Belt-and-braces to make sure it's gone
     * before the next thing draws. */
    spinner_hide(sp);

    char *history = ctrl_strip_dup(ret ? ret : "");
    free(ret);
    tool_render_free(&rr);

    return (struct item){
        .kind = ITEM_TOOL_RESULT,
        .call_id = xstrdup(call->call_id),
        .output = history,
    };
}

/* Top-level dispatch: pick silent or verbose path based on the tool's
 * static silent_preview flag and (for bash) per-call classification of
 * the command. The render_transition handles both directions cleanly —
 * silent → silent is a no-op (cluster continues), any other prior
 * state closes properly before the target opens. */
static struct item dispatch_tool_call(struct render_ctx *r, const struct item *call)
{
    const struct tool *t = find_tool(call->tool_name);
    int is_silent = t && call_is_silent(t, call);
    render_transition(r, is_silent ? RS_CLUSTER : RS_IDLE);
    if (is_silent)
        return dispatch_tool_call_silent(r, call, t);
    return dispatch_tool_call_verbose(r, call);
}

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
    if (!isatty(fileno(stdout)))
        return 0;
    const char *e = getenv("HAX_MARKDOWN");
    if (e && strcmp(e, "0") == 0)
        return 0;
    return 1;
}

/* Whether to render reasoning/CoT deltas live in a dim block. Default
 * off because the volume can be large and most users want only the
 * model's final answer. Backend opt-in is separate (some servers only
 * stream reasoning when explicitly requested — see openrouter); this
 * just decides whether the deltas we receive get drawn. */
static int reasoning_visible(void)
{
    const char *e = getenv("HAX_SHOW_REASONING");
    return e && *e && strcmp(e, "0") != 0;
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
    long env = parse_size(getenv("HAX_CONTEXT_LIMIT"));
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
 * the right snapshot). out is a running sum across the turn's model calls,
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
    } else if (r->last_text_at && !r->idle_shown &&
               monotonic_ms() - r->last_text_at >= TEXT_IDLE_TIMEOUT_MS) {
        /* Skip the leading-space cell when disp says the cursor is
         * already at column 0 or right after a space — the model's
         * last byte is its own breathing room, and an extra pad would
         * read as a stray double-space once the glyph erases. */
        spinner_show_inline_text(r->spinner, !r->disp.at_space_or_bol);
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
     * see". Stream-ending events close the window; tool-call events
     * leave it armed so the idle spinner can fire during a long
     * tool-args generation pause too. Reasoning deltas re-arm it from
     * their own handler when reasoning is visible. */
    if (ev->kind == EV_DONE || ev->kind == EV_ERROR)
        r->last_text_at = 0;
    /* Any event other than EV_RETRY itself means we're past the
     * backoff sleep — clear the countdown so per-event label updates
     * aren't fighting the tick. */
    if (ev->kind != EV_RETRY && r->retry_deadline_at) {
        r->retry_deadline_at = 0;
        spinner_set_label(r->spinner, "working...");
    }

    switch (ev->kind) {
    case EV_TEXT_DELTA: {
        /* Peek-strip leading newlines on the first delta of the stream
         * so a provider's trailing-NL convention doesn't open a stray
         * blank line above the answer. n==0 after strip → keep the
         * spinner up (no state change) until a non-NL byte arrives. */
        const char *s = ev->u.text_delta.text;
        size_t n = strlen(s);
        disp_first_delta_strip(d, &s, &n);
        if (n == 0)
            break;
        render_transition(r, RS_TEXT);
        render_text_chunk(r, s, n);
        break;
    }
    case EV_TOOL_CALL_START:
    case EV_REASONING_ITEM:
        render_flush(r);
        break;
    case EV_TOOL_CALL_DELTA:
    case EV_TOOL_CALL_END:
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
    /* The model loses access to anything not in the conversation history,
     * so any preserved bash temp files referenced by old turns become
     * unreachable garbage. Drop them now rather than letting them sit in
     * /tmp until process exit (or longer if the user kills the process). */
    bash_cleanup_tempfiles();
    agent_print_banner(st->provider, st->sess);
}

int agent_run(struct provider *p, const struct hax_opts *opts)
{
    struct agent_session sess;
    if (agent_session_init(&sess, p, opts) < 0)
        return 1;

    putchar('\n');
    agent_print_banner(p, &sess);
    /* The single bag of rendering state threaded through every render
     * call. disp is embedded (same lifetime as agent_run's frame), so
     * md_emit_to_disp's user pointer is &r.disp; spinner / md are
     * opaque handles owned here and freed below. */
    struct render_ctx r = {.disp = {.trail = 1, .at_space_or_bol = 1},
                           .show_reasoning = reasoning_visible()};
    r.spinner = spinner_new("working...");
    r.md = markdown_enabled() ? md_new(md_emit_to_disp, &r.disp, display_width()) : NULL;
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
     * frames here — agent_state never outlives agent_run. */
    struct agent_state state = {.sess = &sess, .provider = p, .tlog = tlog};
    /* Initialize once — captures the canonical termios baseline and starts
     * the watcher thread. Idempotent; safe even when stdin/stdout aren't
     * ttys (becomes a no-op in that case). */
    interrupt_init();

    const char *prompt = locale_have_utf8() ? PROMPT_UTF8 : PROMPT_ASCII;

    /* The render state in r (state + cluster sub-state) lives across
     * the inner loop so RS_CLUSTER can span consecutive silent tool
     * calls (read/grep/find...) without intervening blank lines.
     * End-of-turn cleanup unconditionally transitions back to RS_IDLE,
     * so leftover state from a prior turn is impossible by
     * construction. */

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
         * never reach the model. Caught before history-add so recognized
         * /-prefixed lines don't pollute up-arrow recall. Lines that
         * look like commands but aren't (e.g. "/tmp/foo" — the
         * dispatcher's bareword check) return SLASH_NOT_A_COMMAND and
         * fall through to the regular model path below. */
        struct slash_ctx sctx = {.state = &state};
        if (slash_dispatch(line, &sctx) != SLASH_NOT_A_COMMAND) {
            free(line);
            r.disp.trail = 1;
            continue;
        }

        input_history_add(input, line);

        /* Mark the turn boundary just before the user message, not just
         * before the model request that consumes it. The transcript
         * renderer treats a TURN_BOUNDARY as the start-of-turn rule;
         * placing it here puts the user's input under its own turn
         * header (turn 1 = user types + first model response). The
         * inner loop's subsequent iterations insert their own
         * boundaries for follow-up round-trips after tool dispatch. */
        agent_session_add_user(&sess, line);
        free(line);
        /* Flush the prompt to the log immediately, before we hand
         * control to the provider. If the stream hangs or the process
         * is killed, the user prompt that triggered the in-flight call
         * is preserved on disk for post-mortem reading. */
        transcript_log_append(tlog, sess.items, sess.n_items);
        /* input_readline left the cursor at column 0 of a fresh row. */
        r.disp.trail = 1;

        /* Aggregated across every model call this user turn produces.
         * ctx and cached track the latest reported value (= current window
         * state, since each call's input subsumes the prior call's prefix);
         * out is a running sum so the summary reflects total tokens
         * generated in response to this prompt. -1 means "no call reported
         * this number yet". */
        long turn_ctx = -1, turn_out = -1, turn_cached = -1;
        int turn_errored = 0;
        int turn_interrupted = 0;

        /* Arm the watcher for the duration of the inner loop — Esc from
         * here on aborts the stream or running tool. Cleared first so a
         * stray Esc from a previous turn (e.g. user typed Esc during
         * readline editing) doesn't auto-cancel this one. */
        interrupt_clear();
        interrupt_arm();
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
                transcript_log_append(tlog, sess.items, sess.n_items);
            }
            first_inner = 0;
            struct context ctx = agent_session_context(&sess);

            render_stream_begin(&r);

            if (r.md)
                md_reset(r.md, display_width());
            struct turn t;
            turn_init(&t);
            r.disp.saw_text = 0;
            struct event_ctx ec = {.r = &r, .turn = &t, .usage = {-1, -1, -1}};
            /* agent_stream_tick combines cancel (Esc) with idle
             * detection: ~1Hz from libcurl's progress callback inside
             * curl_easy_perform, plus on every received chunk. */
            p->stream(p, &ctx, sess.model, on_event, &ec, agent_stream_tick, &r);

            render_stream_end(&r);

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
                transcript_log_append(tlog, sess.items, sess.n_items);
                turn_errored = 1;
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
                turn_ctx = ec.usage.input_tokens + ec.usage.output_tokens;
            if (ec.usage.output_tokens >= 0)
                turn_out = (turn_out < 0 ? 0 : turn_out) + ec.usage.output_tokens;
            if (ec.usage.cached_tokens >= 0)
                turn_cached = ec.usage.cached_tokens;

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
                turn_interrupted = 1;
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
            transcript_log_append(tlog, sess.items, sess.n_items);

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
                turn_interrupted = 1;
                break;
            }
        }
        interrupt_disarm();

        /* Close any open render state before post-turn output — a
         * still-running cluster spinner racing with notify_attention's
         * OSC-9 would corrupt the escape sequence. */
        render_transition(&r, RS_IDLE);

        /* Catch-all flush. The per-round-trip append above covers tool
         * paths; this one picks up the no-tool exit (assistant message
         * only) and the synthesized-results interrupt path. Idempotent
         * when there are no new items — _append no-ops on n_items <= n_written. */
        transcript_log_append(tlog, sess.items, sess.n_items);

        if (turn_interrupted) {
            render_open_block(&r);
            disp_raw(ANSI_DIM);
            disp_printf(&r.disp, "%s", INTERRUPT_MARKER);
            disp_raw(ANSI_RESET);
            disp_putc(&r.disp, '\n');
            fflush(stdout);
        }

        if (!turn_errored)
            display_usage(&r, p, turn_ctx, turn_out, turn_cached);

        /* Ping the terminal so the user gets a notification / dock
         * bounce when hax is back to idle. Skipped on Esc-interrupt
         * since the user just pressed a key — they're already at the
         * terminal. Errored turns still notify: the user needs to
         * know the request bounced. */
        if (!turn_interrupted)
            notify_attention();
    }

    spinner_free(r.spinner);
    input_free(input);
    if (r.md)
        md_free(r.md);
    transcript_log_close(tlog);
    agent_session_free(&sess);
    return 0;
}
