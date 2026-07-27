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
#include "agent_loop.h"
#include "catalog.h"
#include "model_meta.h"
#include "compact.h"
#include "config.h"
#include "file_mention.h"
#include "history.h"
#include "paste_image.h"
#include "select.h"
#include "session.h"
#include "slash.h"
#include "tool.h"
#include "transcript.h"
#include "util.h"
#include "render/disp.h"
#include "render/markdown.h"
#include "render/render_ctx.h"
#include "render/spinner.h"
#include "system/spawn.h"
#include "system/tempfiles.h"
#include "terminal/ansi.h"
#include "terminal/input.h"
#include "terminal/interrupt.h"
#include "terminal/notify.h"
#include "terminal/theme.h"
#include "terminal/ui.h"
#include "terminal/vt_resolve.h"

/* The ASCII fallback is used when locale_init_utf8() couldn't establish a
 * UTF-8 LC_CTYPE — wcwidth() under a non-UTF-8 locale would mis-account
 * the multibyte glyph and break cursor positioning. Composed at runtime
 * (into `buf`) because the accent color comes from the active theme. */
static const char *build_prompt(char *buf, size_t n)
{
    if (locale_have_utf8())
        snprintf(buf, n, "%s" ANSI_BOLD "❯" ANSI_BOLD_OFF "%s ", theme_open(THEME_ACCENT),
                 theme_close(THEME_ACCENT));
    else
        snprintf(buf, n, ANSI_BOLD ">" ANSI_BOLD_OFF " ");
    return buf;
}

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
    struct disp *d = user;
    if (is_raw)
        fwrite(bytes, 1, n, disp_sink(d));
    else
        disp_write(d, bytes, n);
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

/* Whether to render reasoning/CoT deltas live in a dim block. Default
 * off because the volume can be large and most users want only the
 * model's final answer. Backend opt-in is separate (some servers only
 * stream reasoning when explicitly requested — see openrouter); this
 * just decides whether the deltas we receive get drawn. */
static int reasoning_visible(void)
{
    return config_bool("show_reasoning");
}

void agent_display_refresh(struct agent_state *st)
{
    theme_init();
    struct render_ctx *r = st->r;
    r->show_reasoning = reasoning_visible();
    if (r->md) {
        md_free(r->md);
        r->md = NULL;
    }
    if (markdown_enabled())
        r->md = md_new(md_emit_to_disp, &r->disp, md_cols());
}

double agent_session_spend(const struct session_stats *t, int *approx)
{
    return spend_total(&t->spend, approx);
}

/* Request counts and window snapshots stay with callers because compaction
 * accounts them differently from ordinary continuation turns. */
static void stats_account_usage(struct session_stats *stats, const struct stream_usage *usage,
                                const struct provider *p, const char *model)
{
    if (usage->input_tokens >= 0)
        stats->input_tokens += usage->input_tokens;
    if (usage->output_tokens >= 0)
        stats->output_tokens += usage->output_tokens;
    if (usage->cached_tokens > 0)
        stats->cached_tokens += usage->cached_tokens;
    if (usage->cache_write_tokens > 0)
        stats->cache_write_tokens += usage->cache_write_tokens;
    if (usage->input_tokens > 0)
        stats->uncached_tokens += usage_uncached_input(usage, p, model);
    spend_account(&stats->spend, usage, p, model);
}

/* Dim per-user-turn stats line: "42s · 8.9k / 256k (3%) · $0.042",
 * shown once per user turn so multi-step tool runs collapse into a single
 * summary instead of bracketing every intermediate response. Distinct from
 * /usage (the provider's account report), /session (this session's
 * totals), and the transcript's per-request ITEM_TURN_USAGE footers —
 * this is the ambient per-turn view.
 *
 * ctx reflects the last response (= current window state — each call's
 * input subsumes the prior call's prefix, so the latest value is the
 * right snapshot). elapsed_ms is this user turn's wall time; spend is the
 * *session's* cumulative cost — provider-reported, plus the catalog
 * estimate for responses that reported none (agent_session_spend), the
 * latter marking the figure "~$". Field selection and formatting live in
 * format_stats_segments, shared with oneshot's exit summary.
 *
 * Segments wrap at the " · " seams against the content width, so a narrow
 * terminal reflows between fields instead of the terminal hard-wrapping
 * mid-number. Width is sampled at print time; the line is scrollback, so
 * there's no post-print reflow obligation. */
static void display_stats_line(struct render_ctx *r, const struct provider *p, const char *model,
                               long ctx, long elapsed_ms, const struct session_stats *stats)
{
    int approx = 0;
    double spend = agent_session_spend(stats, &approx);
    char segs[STATS_SEGS_MAX][STATS_SEG_LEN];
    int n =
        format_stats_segments(segs, ctx, model_meta_context(p, model), elapsed_ms, spend, approx);
    if (n == 0)
        return;

    struct disp *d = &r->disp;
    render_open_block(r);
    disp_raw(d, ANSI_DIM);

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
    disp_raw(d, ANSI_RESET);
    disp_putc(d, '\n');
    disp_flush(d);
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
    /* Acknowledge a soft Esc: nothing visibly changes until the loop
     * reaches its seam, so the spinner label is the confirmation that
     * the pause was registered — and the reminder that a second Esc
     * escalates. Force-set every tick: a keypress acknowledgment can't
     * wait out settle hysteresis against per-delta "thinking" requests,
     * and set_label is idempotent (redraws only on actual change) while
     * discarding any deferred request that landed between ticks. */
    if (interrupt_soft_requested() && !interrupt_requested()) {
        spinner_set_label(r->spinner, "pausing", "pausing... (esc again to interrupt)");
        /* A request that has produced no content yet — still prefilling,
         * sent in the race window right after the previous seam, or
         * sleeping out a retry backoff — is cancelled outright: there is
         * nothing streamed for the pause to lose, and letting it run would
         * delay the pause by a whole turn, tools included. The loop turns
         * the resulting empty turn into AGENT_LOOP_PAUSED; resume simply
         * re-sends the request. */
        if (!r->stream_content_seen)
            return 1;
    }
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

static int render_on_event(const struct stream_event *ev, void *user)
{
    struct render_ctx *r = user;
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

    /* First model output of any kind closes the soft-interrupt free-
     * cancellation window (see agent_stream_tick): from here on a pause
     * waits for the turn's seam instead of aborting the stream. */
    if (ev->kind == EV_TEXT_DELTA || ev->kind == EV_REASONING_DELTA ||
        ev->kind == EV_REASONING_ITEM || ev->kind == EV_TOOL_CALL_START)
        r->stream_content_seen = 1;

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
        disp_flush(d);
        break;
    case EV_ERROR:
        /* Full close before drawing the error block. render_open_block's
         * RS_IDLE transition flushes md's tail if RS_TEXT was open, so
         * partial pre-error text appears just above the error line. */
        render_open_block(r);
        disp_raw(d, theme_open(THEME_ERROR));
        disp_printf(d, "[error: %s]", ev->u.error.message);
        disp_raw(d, ANSI_RESET);
        disp_putc(d, '\n');
        disp_flush(d);
        break;
    }

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

/* Defined with the rest of the banner below; the paged history view opens
 * with its own two banner rows. */
static const char *banner_bar(char *buf, size_t n);
static void print_banner_identity(FILE *out, const struct provider *p,
                                  const struct agent_session *s);

/* Open the user's pager. Falls back to `less -R` so the ANSI both views
 * carry renders as color rather than escape soup. */
static int view_pager_open(struct spawn_pipe *sp)
{
    const char *pager = getenv("PAGER");
    if (!pager || !*pager)
        pager = "less -R";
    /* spawn_pipe_open shields the parent from terminal-generated
     * SIGINT/SIGQUIT (so Ctrl-C in the pager exits the pager, not
     * hax) and from SIGPIPE on the write path (so quitting the pager
     * early gives EPIPE rather than killing hax). The child sees all
     * three at default disposition, so less behaves normally. */
    return spawn_pipe_open(sp, pager);
}

/* Adapt paste_image_capture (stateless) to the editor's hook signature. */
static char *paste_cb(void *user)
{
    (void)user;
    return paste_image_capture();
}

/* Terminal-shortcut pastes and drag-and-drop arrive as bracketed-paste
 * bodies, never through the Ctrl-V hook — run the same file:// URI
 * conversion on them via the editor's body filter. */
static char *paste_filter_cb(const char *text, void *user)
{
    (void)user;
    return paste_image_uris_to_paths(text);
}

/* Both view hooks take the live agent_state as their user pointer: reading
 * the session and provider through it is what keeps a view current, since
 * the items vector moves as xrealloc grows it and /provider replaces the
 * provider outright. Lifetime is agent_run's frame — input_free runs before
 * it returns (see the cleanup at the end), so a stray keypress can never
 * reach a dead frame. */
static void show_transcript_cb(void *user)
{
    const struct agent_state *st = user;
    const struct agent_session *s = st->sess;
    struct spawn_pipe sp;
    if (view_pager_open(&sp) < 0)
        return;
    transcript_render(sp.w, s->sys, s->tools, s->n_tools, s->items, s->n_items);
    spawn_pipe_close(&sp);
}

/* Ctrl-O: the whole conversation in the on-screen idiom, paged.
 *
 * Rendered through a private render_ctx rather than the live one: this
 * must not disturb the real display's block/held bookkeeping, and it
 * needs its own markdown stream (the live one may be mid-block). No
 * spinner — every spinner call is NULL-safe, and an animation would be
 * meaningless in a file.
 *
 * The pipeline paints with the cursor (markdown retro-wrap, the tool
 * block's closing overprint), so it renders into a memory stream first
 * and the bytes go through vt_resolve, which settles them into the plain
 * rows a terminal would be showing. Rendering completes before the pager
 * is spawned, so `less` sees one clean stream and never sits waiting on a
 * half-rendered conversation.
 *
 * Reasoning follows the live setting, read per invocation: turning
 * show_reasoning on with /config and pressing Ctrl-O shows the reasoning
 * for turns that were rendered without it.
 *
 * Opens with two banner rows — the identity row, so the view starts the way
 * the session did on screen and names the provider, model, effort and
 * stance the conversation is on, then this view's own label. The selection
 * named is the *current* one: a mid-conversation switch is a UI hint only
 * and never enters history (see agent_apply_settings), so no view can
 * reconstruct one — the same compromise the Ctrl-T transcript makes with the
 * system prompt. */
static void show_history_cb(void *user)
{
    const struct agent_state *st = user;
    const struct agent_session *s = st->sess;
    struct render_ctx r = {.show_reasoning = reasoning_visible()};
    char *buf = NULL;
    size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    if (!mem)
        return;
    r.disp.out = mem;
    if (markdown_enabled())
        r.md = md_new(md_emit_to_disp, &r.disp, md_cols());

    print_banner_identity(mem, st->provider, s);
    /* Second banner row: which of the two paged views this is, in the slot
     * the REPL's key-tip row occupies. A label, not advice — how to leave
     * the view belongs to $PAGER, which hax doesn't control. The prompt
     * count is the one thing the pager can't convey (its indicator is byte
     * progress, not conversation size); dropped when there is nothing to
     * count, since the note below then says so. */
    char bar[32];
    size_t prompts = agent_user_turn_count(s);
    if (prompts > 0)
        fprintf(mem, "%s " ANSI_DIM "conversation history · %zu prompt%s" ANSI_BOLD_OFF "\n",
                banner_bar(bar, sizeof(bar)), prompts, prompts == 1 ? "" : "s");
    else
        fprintf(mem, "%s " ANSI_DIM "conversation history" ANSI_BOLD_OFF "\n",
                banner_bar(bar, sizeof(bar)));
    /* The banner rows write straight at the sink (bypassing disp) and end on
     * a fresh row: trail = 1 tells the first block separator to add the
     * single blank line the live screen has under the banner. */
    r.disp.trail = 1;

    if (s->n_items == 0) {
        render_open_block(&r);
        disp_raw(&r.disp, ANSI_DIM);
        disp_printf(&r.disp, "(nothing in this conversation yet)");
        disp_raw(&r.disp, ANSI_RESET);
        disp_putc(&r.disp, '\n');
    } else {
        history_render(&r, HISTORY_FULL, s->items, s->n_items, 0);
    }
    /* Close the last block so a trailing markdown tail is flushed and the
     * final row is terminated, exactly as the replay path does. */
    render_transition(&r, RS_IDLE);
    disp_emit_held(&r.disp);
    md_free(r.md);
    fclose(mem);

    struct spawn_pipe sp;
    if (view_pager_open(&sp) == 0) {
        vt_resolve(buf, len, sp.w);
        spawn_pipe_close(&sp);
    }
    free(buf);
}

/* The banner's leading chrome bar, composed into `buf` (the glyph carries
 * theme escapes, so it can't be a literal) and returned for inline use in
 * a format argument — same shape as build_prompt. */
static const char *banner_bar(char *buf, size_t n)
{
    snprintf(buf, n, "%s▌%s", theme_open(THEME_CHROME), theme_close(THEME_CHROME));
    return buf;
}

/* The banner's first row: the bar, the name, the active stance, and the
 * selection. Split out from agent_print_banner because it is the half that
 * makes sense outside the live prompt — the paged history view opens with
 * it, so the view starts the way the session did on screen and says what
 * the conversation is running on. The second row is the key tip, which is
 * advice for the REPL only (and wrong in a pager, where ctrl-d scrolls). */
static void print_banner_identity(FILE *out, const struct provider *p,
                                  const struct agent_session *s)
{
    char bar[32];
    banner_bar(bar, sizeof(bar));
    /* Active preset stance, the one colored token on an otherwise dim line.
     * Load-bearing, not decoration: a preset may have swapped the system
     * prompt, and the name is the only at-a-glance signal that a stance —
     * persona included — is in effect. It carries the stance hue (the same
     * one the preset's tint gives the model's markdown), so the banner and
     * the replies below it agree on which persona this terminal is running.
     * The bar stays chrome: hax is still hax.
     *
     * SGR 22 opens a hole in the surrounding dim run and ANSI_DIM closes it
     * again — the token must not be dimmed, since faint over an indexed
     * color is exactly what THEME_CHROME_DIM exists to avoid. */
    const char *preset = config_str("preset");
    char *stance = (preset && *preset)
                       ? xasprintf(ANSI_BOLD_OFF "%s[%s]%s" ANSI_DIM " ", theme_open(THEME_STANCE),
                                   preset, theme_close(THEME_STANCE))
                       : xstrdup("");
    if (!p) {
        /* No provider could be constructed (the configured/default one isn't
         * usable — e.g. codex not logged in). The REPL still starts; point
         * the user at /provider to choose a working one. */
        fprintf(out,
                "%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM
                "%s› no provider — use /provider" ANSI_BOLD_OFF "\n",
                bar, stance);
        free(stance);
        return;
    }
    const char *name = p->name ? p->name : "?";
    const char *model_label = s->model_label ? s->model_label : s->model;
    if (!s->model || !*s->model)
        /* No model resolved (a provider with no default, nothing configured
         * yet). The REPL still starts; point the user at /model. */
        fprintf(out,
                "%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM
                "%s› %s · no model — use /model" ANSI_BOLD_OFF "\n",
                bar, stance, name);
    else if (s->effort)
        fprintf(out,
                "%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM "%s› %s · %s · %s" ANSI_BOLD_OFF
                "\n",
                bar, stance, name, model_label, s->effort);
    else
        fprintf(out,
                "%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM "%s› %s · %s" ANSI_BOLD_OFF "\n",
                bar, stance, name, model_label);
    free(stance);
}

/* Caller is expected to have already emitted the leading blank-line
 * gap (slash_dispatch does this for /new; agent_run does it at startup
 * before the first call). The banner itself is just two output rows so
 * it composes cleanly with whatever surrounded the call. */
void agent_print_banner(const struct provider *p, const struct agent_session *s)
{
    print_banner_identity(stdout, p, s);
    char bar[32];
    printf("%s " ANSI_DIM "ctrl-d quit · try /help" ANSI_BOLD_OFF "\n",
           banner_bar(bar, sizeof(bar)));
}

int agent_apply_settings(struct agent_state *st, struct provider *p, int announce)
{
    struct agent_session *s = st->sess;
    struct provider *old = (struct provider *)st->provider;
    int provider_changed = p != old;
    /* Snapshot the model before reconfigure overwrites it, to tell a real
     * /model change from a /provider or /effort apply that left it the same. */
    char *prev_model = s->model ? xstrdup(s->model) : NULL;
    if (agent_session_reconfigure(s, p) != 0) {
        free(prev_model);
        return -1;
    }

    /* Past the last failure return, so the display is only re-resolved once
     * the new settings are certain to stick — a caller that rolls its config
     * snapshot back after a failure above never drew under them. Every
     * selection commit lands here, which is what keeps the tint in step with
     * the stance: /preset applying one, and /provider, /model, or /effort
     * exiting it. The banner printed below is the first thing to need it. */
    agent_display_refresh(st);

    /* Install a prospective provider only after its settings resolve. On
     * failure above, the live provider remains untouched and ownership of a
     * prospective p stays with the caller. Each spend record carries its own
     * provider/model stamp, so no bookkeeping needs settling before the old
     * provider is destroyed. */
    if (provider_changed) {
        st->provider = p;
        if (old)
            old->destroy(old);
    }

    /* A model or provider switch invalidates everything known about the
     * previous selection — window, output cap, modalities, effort levels.
     * Provider identity matters even when the model id is unchanged: the
     * fresh provider's constructor may have probed its default model, or
     * skipped the probe while /provider transactionally hid the outgoing
     * model. A bare /effort tweak skips the churn, and so does a /model
     * pick whose entry the picker already handed over. */
    int model_changed = (prev_model == NULL) != (s->model == NULL) ||
                        (prev_model && s->model && strcmp(prev_model, s->model) != 0);
    free(prev_model);
    if (provider_changed || model_changed)
        model_meta_refresh(p, s->model);

    /* reconfigure rebuilt sess->sys (its Environment section names the new
     * model), so re-key the HAX_TRANSCRIPT mirror to it: rewrite the header and replay
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
    session_log_set_meta(st->slog, provider_log_name(p), s->model, s->effort, config_str("preset"));

    /* Silent apply: the caller says what changed. `/new <preset>` applies
     * before it resets, so the only thing on screen is the fresh
     * conversation's banner — already carrying the stance this just set. */
    if (!announce)
        return 0;

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
    /* The stance rides along like it does in the banner: a preset may have
     * swapped the system prompt, so "switched to mock · m" alone would
     * understate what changed (this covers /preset and a /resume restore). */
    const char *preset = config_str("preset");
    char *stance = (preset && *preset) ? xasprintf("[%s] ", preset) : xstrdup("");
    char *label = s->effort ? xasprintf("switched to %s%s · %s · %s", stance,
                                        p->name ? p->name : "?", model_label, s->effort)
                            : xasprintf("switched to %s%s · %s", stance, p->name ? p->name : "?",
                                        model_label);
    free(stance);

    render_open_block(st->r);
    disp_raw(&st->r->disp, ANSI_DIM);
    disp_printf(&st->r->disp, "%s", label);
    disp_raw(&st->r->disp, ANSI_RESET);
    disp_putc(&st->r->disp, '\n');
    disp_flush(&st->r->disp);
    free(label);
    return 0;
}

/* Any wholesale history rewrite invalidates a pending resumable turn: the
 * trailing state an empty send would continue no longer exists (or no
 * longer means what the hint claims). A deferred compaction debt dies with
 * it — it described the size of history that is gone. */
static void resume_clear(struct agent_state *st)
{
    st->resume = AGENT_RESUME_NONE;
    st->resume_marked = 0;
    st->compact_deferred = 0;
}

void agent_new_conversation(struct agent_state *st)
{
    resume_clear(st);
    agent_session_reset(st->sess);
    transcript_log_reset(st->tlog, st->sess->sys, st->sess->tools, st->sess->n_tools);
    /* Rotate to a fresh session file so the cleared conversation doesn't
     * keep appending to the prior session's record. */
    session_log_reset(st->slog);
    /* The model loses access to anything not in the conversation history,
     * so tracked temp files referenced by old turns (bash spills, pasted
     * images) become unreachable garbage. Drop them now rather than letting
     * them sit in /tmp until process exit (or longer if the user kills the
     * process). */
    tempfiles_cleanup();
    /* A fresh conversation starts its /session ledger at zero too. */
    spend_free(&st->stats.spend);
    memset(&st->stats, 0, sizeof(st->stats));
    agent_print_banner(st->provider, st->sess);
}

/* One dim line directly above the prompt while a resumable turn is
 * pending, in the transcript's bracket idiom. Re-emitted on every prompt
 * draw (not just once at the stop) so the explanation survives slash
 * command output scrolling it away: the invariant is that whenever an
 * empty send would continue the turn, the line saying so is on screen. */
static void render_resume_hint(struct render_ctx *r, enum agent_resume resume)
{
    const char *what;
    const char *action = "enter to continue";
    switch (resume) {
    case AGENT_RESUME_PAUSED:
        what = "paused";
        break;
    case AGENT_RESUME_MAX_TURNS:
        what = "max turns reached";
        break;
    case AGENT_RESUME_INTERRUPTED:
        what = "interrupted";
        break;
    case AGENT_RESUME_ERROR:
        what = "provider error";
        action = "enter to retry";
        break;
    default:
        return;
    }
    disp_raw(&r->disp, ANSI_DIM);
    disp_printf(&r->disp, "[%s — %s]", what, action);
    disp_raw(&r->disp, ANSI_RESET);
    disp_putc(&r->disp, '\n');
    disp_putc(&r->disp, '\n'); /* one blank line between hint and prompt */
    /* Commit the newlines instead of leaving them held: the editor paints
     * the prompt with \r + erase-line on the cursor's row, which would wipe
     * the hint if the cursor were still parked at the end of its line. */
    disp_emit_held(&r->disp);
    disp_flush(&r->disp);
}

/* Replay the last user turn — the final user message plus every turn the
 * model produced in response — through the live display pipeline, so
 * resuming looks like the conversation never scrolled away rather than
 * dropping the user at a bare prompt. Assistant text and (when shown)
 * reasoning render at full fidelity; tool calls collapse to dim one-line
 * headers. Deliberately one turn and no tool output: the replay lands
 * *below* whatever is already on screen, so a bigger budget would push the
 * real history — which after /undo or /fork is exactly what the user is
 * looking at — out of view. The whole conversation, tool output included,
 * is one Ctrl-O away; the dim rule says so and counts what it isn't
 * showing. `lead` is the verb clause inside that rule ("resumed", "undid 2
 * turns", "forked"), so /resume, /undo and /fork share one replay with
 * different framing.
 *
 * Anchored on the last user message the view draws, not the last
 * TURN_BOUNDARY: one user prompt can span several round-trips (turns), and we
 * want them all. Interactive-only — gated on both stdin and stdout being TTYs (same as
 * cursor_supported()), so a non-interactive run (`printf … | hax --resume`,
 * or any piped stdin/stdout) renders nothing extra before processing input. */
/* A user item the replay can start from: one the view draws something for. A
 * compaction seed qualifies — it renders as the "conversation compacted"
 * marker, and after a compaction it may be the only user item left. An
 * empty-send continuation draws nothing at all (src/history.c), so anchoring
 * there would begin the replay *below* the prompt that started the work,
 * dropping it from the turn and counting it among the earlier messages. */
static int is_replay_anchor(const struct item *it)
{
    return it->kind == ITEM_USER_MESSAGE && it->origin != ITEM_ORIGIN_CONTINUATION;
}

static void replay_user_turn(struct render_ctx *r, const struct agent_session *s, const char *lead)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return;

    size_t anchor = 0;
    int found = 0;
    size_t earlier = 0;
    for (size_t i = s->n_items; i-- > 0;) {
        if (!is_replay_anchor(&s->items[i]))
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
    disp_raw(&r->disp, ANSI_DIM);
    if (earlier > 0)
        disp_printf(&r->disp, "── %s · %zu earlier message%s · ctrl-o for full history ──", lead,
                    earlier, earlier == 1 ? "" : "s");
    else
        disp_printf(&r->disp, "── %s · ctrl-o for full history ──", lead);
    disp_raw(&r->disp, ANSI_RESET);
    disp_putc(&r->disp, '\n');

    if (found)
        history_render(r, HISTORY_BRIEF, s->items, s->n_items, anchor);

    /* Land at column 0 with one committed trailing newline, so the prompt
     * that follows is separated by a clean blank line. md_flush leaves the
     * last row open (no trailing \n), so terminate it explicitly; then
     * commit any held newline. This makes the slash path's `trail = 1`
     * reset (after /resume) and the startup loop's separator both correct. */
    render_transition(r, RS_IDLE);
    if (r->disp.trail == 0 && r->disp.held == 0)
        disp_putc(&r->disp, '\n');
    disp_emit_held(&r->disp);
    disp_flush(&r->disp);
}

void agent_resume_session(struct agent_state *st, const char *path)
{
    /* /resume can race a daily sweep started by another process just like a
     * startup resume; claim activity before reading the selected file. */
    (void)session_touch(path);
    struct agent_session *s = st->sess;
    struct item *loaded = NULL;
    size_t nl = 0;
    struct session_meta meta;
    if (session_load(path, &loaded, &nl, &meta) != 0 || nl == 0) {
        free(loaded);
        session_meta_free(&meta);
        ui_error("could not read session");
        /* The error line is now the last thing printed, on its own fresh
         * row. /resume set trail = 2 for the picker that's now moot; correct
         * it to the post-error state (one trailing newline) so the next
         * prompt still gets its blank-line separation. */
        st->r->disp.trail = 1;
        return;
    }

    /* Leave the prior session's file before anything can stamp it: the
     * restore below reconfigures the run, and that must not be recorded as a
     * switch in the conversation the user just left. */
    session_log_close(st->slog);
    st->slog = NULL;

    /* Continue on what this conversation was using, like --resume does at
     * startup. Deliberately before the history swap, so the confirmation it
     * prints is chosen against the conversation being *left*: from an empty
     * one — the startup banner still on screen above, asserting settings this
     * is about to invalidate — that's a fresh banner, and the replay below
     * then follows it exactly as it does at startup. Mid-conversation it's
     * the dim "switched to …" line instead, where a banner would falsely
     * imply a reset. Also before the log/transcript re-key below, so both are
     * opened against the restored system prompt and model. */
    select_restore_session(st, meta.provider, meta.model, meta.effort, meta.preset);

    /* Swap the loaded conversation in for the current one. */
    for (size_t i = 0; i < s->n_items; i++)
        item_free(&s->items[i]);
    free(s->items);
    s->items = loaded;
    s->n_items = nl;
    s->cap_items = nl;

    /* Continue the resumed file rather than the prior one, and re-key the
     * transcript mirror to the restored history (reset writes the header,
     * the append replays the items). The log opens against what the file
     * records, so the set_meta that follows records a difference — a restore
     * that couldn't be applied — as the switch it is.
     *
     * Asked again rather than inherited from startup: select_restore_session
     * just put the run on the resumed conversation's provider, so a run that
     * began on the dev backend — recording nothing — goes on recording this
     * one if the session it opened belongs to a real one. */
    if (agent_recording_enabled(st->provider))
        st->slog =
            session_log_resume(path, meta.provider, meta.model, meta.effort, meta.preset, nl);
    /* Stage the live selection against it. After a successful restore the two
     * agree and this is a no-op; after a failed one it records the fallback
     * the run is really on — but only once a turn is sent under it, so
     * opening a conversation whose backend is unavailable and quitting leaves
     * its recorded setup intact. */
    session_log_set_meta(st->slog, provider_log_name(st->provider), s->model, s->effort,
                         config_str("preset"));
    session_meta_free(&meta);
    transcript_log_reset(st->tlog, s->sys, s->tools, s->n_tools);
    transcript_log_append(st->tlog, s->items, s->n_items);

    /* No tempfiles_cleanup() here, unlike /new: the loaded history can
     * reference tracked files — resuming the parent of a /fork shares the
     * fork's retained prefix, pasted images and bash spills included. The
     * prior conversation's files flush at /new, compaction, or exit. (If
     * accumulation ever matters, scan the loaded history and drop only
     * unreferenced entries.) */
    resume_clear(st);
    replay_user_turn(st->r, s, "resumed");
}

/* A user item the user actually typed — what /undo, /fork and the history
 * banner mean by a turn. Compaction seeds and empty-send continuations are
 * ours: neither was typed, and a continuation extends the turn already
 * counted rather than opening one (see the empty-send branch in agent_run).
 * session.c's line_is_typed_prompt is the same rule over the JSONL mirror,
 * and the two have to agree — /undo derives the file cut from one and the
 * item count from the other. */
static int is_typed_prompt(const struct item *it)
{
    return it->kind == ITEM_USER_MESSAGE && it->origin == ITEM_ORIGIN_NONE;
}

/* Item index of the 0-based typed user message `turn`, or -1 when out of
 * range. The truncation point for /undo and /fork is this index, backed up
 * over the turn_boundary that opens the turn (so the boundary goes too). */
static int turn_item_index(const struct agent_session *s, size_t turn, size_t *out)
{
    size_t seen = 0;
    for (size_t i = 0; i < s->n_items; i++) {
        if (is_typed_prompt(&s->items[i])) {
            if (seen == turn) {
                *out = i;
                return 0;
            }
            seen++;
        }
    }
    return -1;
}

size_t agent_user_turn_count(const struct agent_session *s)
{
    size_t n = 0;
    for (size_t i = 0; i < s->n_items; i++)
        if (is_typed_prompt(&s->items[i]))
            n++;
    return n;
}

const char *agent_user_turn_text(const struct agent_session *s, size_t turn)
{
    size_t idx;
    if (turn_item_index(s, turn, &idx) != 0)
        return NULL;
    return s->items[idx].text;
}

/* Shared tail of /undo and /fork: stash the discarded prompt for recall, free
 * history items from `cut` onward, clear the current-window snapshot when turns
 * were discarded, re-key the transcript mirror, and replay the new tail behind
 * `lead`. `cut` is the item index to truncate at; `turn` names the first
 * discarded user turn (its prompt is the one to re-edit). */
static void reshape_after_cut(struct agent_state *st, size_t cut, size_t turn, const char *lead)
{
    struct agent_session *s = st->sess;
    resume_clear(st);

    /* Capture the discarded prompt before its item is freed. The REPL seeds it
     * into recall after the /undo or /fork command line, so Up-arrow reaches
     * the prompt, not the command (see agent_state.pending_recall). */
    const char *recall = agent_user_turn_text(s, turn);
    free(st->pending_recall);
    st->pending_recall = recall ? xstrdup(recall) : NULL;

    size_t old_n = s->n_items;
    for (size_t i = cut; i < s->n_items; i++)
        item_free(&s->items[i]);
    s->n_items = cut;

    /* The "current window" snapshot (/session's context row) is only ever set
     * from a server-reported response. Once we discard turns it describes a
     * response that's gone, and retained ITEM_TURN_USAGE footers (a compaction
     * summary request's, say) don't reliably represent the new tail — so clear
     * it rather than re-derive, letting /session show a context figure again
     * only after the next real turn. A no-op cut (/fork 0 discards nothing)
     * leaves the deliberate snapshot. Cumulative per-sitting totals always stay. */
    if (cut < old_n) {
        st->stats.last_ctx = 0;
        st->stats.last_limit = 0;
    }

    transcript_log_reset(st->tlog, s->sys, s->tools, s->n_tools);
    transcript_log_append(st->tlog, s->items, s->n_items);
    /* No tempfiles_cleanup() here: it unlinks the whole tracked-file
     * registry, but the retained prefix (all of it for /fork 0) can still hold
     * "output saved to <path>" markers or pasted-image references, and cleanup
     * is all-or-nothing. Leave the discarded turns' files to /new and exit. */
    replay_user_turn(st->r, s, lead);
}

void agent_undo(struct agent_state *st, size_t turn)
{
    struct agent_session *s = st->sess;
    size_t cut;
    if (turn_item_index(s, turn, &cut) != 0)
        return;
    if (cut > 0 && s->items[cut - 1].kind == ITEM_TURN_BOUNDARY)
        cut--;

    size_t removed = agent_user_turn_count(s) - turn;

    /* Truncate the on-disk record first; on I/O failure bail with history
     * intact. The file keeps the old branch and its high-water mark, so
     * truncating memory too would later append onto that stale branch. */
    if (session_log_truncate(st->slog, turn, cut) != 0) {
        ui_error("could not truncate the session file; conversation left unchanged");
        st->r->disp.trail = 1;
        return;
    }

    char lead[64];
    snprintf(lead, sizeof(lead), "undid %zu turn%s", removed, removed == 1 ? "" : "s");
    reshape_after_cut(st, cut, turn, lead);
}

void agent_fork(struct agent_state *st, size_t turn)
{
    struct agent_session *s = st->sess;
    size_t cut;
    if (turn >= agent_user_turn_count(s)) {
        /* Fork at the current tip (/fork 0): keep every turn — a clone onto a
         * new branch, nothing discarded. */
        cut = s->n_items;
    } else {
        if (turn_item_index(s, turn, &cut) != 0)
            return;
        if (cut > 0 && s->items[cut - 1].kind == ITEM_TURN_BOUNDARY)
            cut--;
    }

    /* Fork must preserve the original as a resumable branch, which needs a
     * session file to copy. With recording off (HAX_NO_SESSION) or a
     * materialization failure there is none, so refuse rather than silently
     * degrade into a destructive /undo mislabeled "forked". */
    if (!session_log_materialized(st->slog)) {
        ui_error("/fork needs session recording (it is disabled or unavailable)");
        st->r->disp.trail = 1;
        return;
    }

    /* Copy the prefix into a new file and switch the live logs onto it,
     * leaving the original whole (resumable as the pre-fork branch). */
    const char *src = session_log_path(st->slog);
    char *newpath = NULL;
    if (session_fork_file(src, turn, &newpath) != 0) {
        ui_error("could not create fork");
        st->r->disp.trail = 1;
        return;
    }
    /* Open the new logger before retiring the old one: if it can't be opened,
     * the original stays live and the conversation is untouched (rather than
     * forking into an unrecordable branch). src borrows from st->slog, so it
     * must outlive this call too. It opens against what the copied prefix
     * records and is then stamped with the live selection, so a fork made
     * after a switch the retained turns predate carries that switch onto the
     * branch with its first new turn — resuming the fork then continues on
     * what the user is running. */
    struct session_meta fmeta;
    session_read_meta(newpath, &fmeta);
    struct session_log *newlog =
        session_log_resume(newpath, fmeta.provider, fmeta.model, fmeta.effort, fmeta.preset, cut);
    session_meta_free(&fmeta);
    if (!newlog) {
        unlink(newpath);
        free(newpath);
        ui_error("could not open the fork session file; conversation left unchanged");
        st->r->disp.trail = 1;
        return;
    }
    session_log_set_meta(newlog, provider_log_name(st->provider), s->model, s->effort,
                         config_str("preset"));
    free(newpath);
    session_log_close(st->slog);
    st->slog = newlog;

    reshape_after_cut(st, cut, turn, "forked");
}

/* Compaction events render only through the spinner, but their usage still
 * contributes to /session totals. History assembly and footer capture belong
 * to compact_run. */
struct compact_ev {
    struct session_stats *stats;
    struct render_ctx *render;
    const struct provider *provider;
    const char *model;
};

static int compact_on_event(const struct stream_event *ev, void *user)
{
    struct compact_ev *ce = user;
    const struct stream_usage *usage = NULL;
    if (ev->kind == EV_DONE)
        usage = &ev->u.done.usage;
    else if (ev->kind == EV_ERROR)
        usage = ev->u.error.usage;
    if (!usage)
        return 0;

    stats_account_usage(ce->stats, usage, ce->provider, ce->model);
    return 0;
}

/* Compaction has no seam to pause at — a soft Esc cancels it like a hard
 * one. The transaction is retriable (auto-compact re-triggers, /compact
 * can be re-run), so responsiveness wins over finishing the summary. */
static int compact_tick(void *user)
{
    struct compact_ev *ce = user;
    return agent_stream_tick(ce->render) || interrupt_soft_requested();
}

static int compact_cancelled(void *user)
{
    (void)user;
    interrupt_settle();
    return interrupt_requested() || interrupt_soft_requested();
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
    disp_raw(&r->disp, ANSI_DIM);
    disp_printf(&r->disp, "── %s ──", buf);
    disp_raw(&r->disp, ANSI_RESET);
    disp_putc(&r->disp, '\n');
    disp_flush(&r->disp);
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

    /* Spinner-only stream: compact_run owns event assembly and the history
     * transaction; this wrapper supplies presentation, cancellation, and
     * session accounting. */
    render_stream_begin(r);
    spinner_set_label(r->spinner, "compacting", "compacting...");
    struct compact_ev ce = {
        .stats = &st->stats,
        .render = r,
        .provider = p,
        .model = s->model,
    };
    struct compact_params params = {
        .session = s,
        .provider = p,
        .slog = st->slog,
        .tlog = st->tlog,
        .instructions = instructions,
        .hooks =
            {
                .user = &ce,
                .observe = compact_on_event,
                .tick = compact_tick,
                .cancelled = compact_cancelled,
            },
    };

    interrupt_clear();
    interrupt_arm();
    struct compact_result result;
    compact_run(&params, &result);
    /* Cancelled attempts emit no terminal event, so the transaction reports
     * the authoritative request count separately from usage observation. */
    st->stats.requests += result.attempts;
    interrupt_disarm();
    render_transition(r, RS_IDLE);

    int compacted = result.outcome == COMPACT_COMPLETE;
    /* The "current window" snapshot describes a response whose history no
     * longer exists — clear it like reshape_after_cut does, so threshold
     * checks against it (the resumable-prompt preflight in particular)
     * don't re-compact the fresh seed on stale numbers. /session shows a
     * context figure again after the next reported response. */
    if (compacted) {
        st->stats.last_ctx = 0;
        st->stats.last_limit = 0;
        /* Any successful compaction — manual included — settles a deferred
         * end-of-turn pass: the oversized history it referred to is gone. */
        st->compact_deferred = 0;
    }
    /* A manual /compact replaced the trailing turn a pending resume would
     * have continued with the summary seed; the seed already prompts the
     * model on its own, so drop the resumable state rather than have an
     * empty send bolt a stale [continue] onto it. The auto path only runs
     * where no resumable state exists (mid-loop, or after COMPLETE). */
    if (compacted && !is_auto)
        resume_clear(st);
    switch (result.outcome) {
    case COMPACT_COMPLETE:
        compact_notice(r, "conversation compacted");
        break;
    case COMPACT_CANCELLED:
        compact_notice(r, "compaction cancelled");
        break;
    case COMPACT_PROVIDER_ERROR:
        compact_notice(r, "compaction failed: %s",
                       result.error_message ? result.error_message : "stream failed");
        break;
    case COMPACT_NO_SUMMARY:
        compact_notice(r, "compaction produced no summary");
        break;
    case COMPACT_NO_PROVIDER:
        if (!is_auto)
            compact_notice(r, "no provider selected — use /provider");
        break;
    case COMPACT_NO_MODEL:
        if (!is_auto)
            compact_notice(r, "no model selected — use /model (or /provider)");
        break;
    case COMPACT_EMPTY:
        if (!is_auto)
            compact_notice(r, "nothing to compact");
        break;
    }
    compact_result_destroy(&result);
    return compacted;
}

struct repl_loop_ctx {
    struct agent_state *state;
};

static int repl_loop_on_event(const struct stream_event *ev, void *user)
{
    struct repl_loop_ctx *ctx = user;
    return render_on_event(ev, ctx->state->r);
}

static int repl_loop_tick(void *user)
{
    struct repl_loop_ctx *ctx = user;
    return agent_stream_tick(ctx->state->r);
}

static void repl_loop_turn_begin(void *user)
{
    struct repl_loop_ctx *ctx = user;
    struct render_ctx *r = ctx->state->r;
    render_stream_begin(r);
    if (r->md)
        md_reset(r->md, md_cols());
    r->disp.saw_text = 0;
    r->stream_content_seen = 0;
}

static void repl_loop_turn_end(const struct agent_loop_turn *loop_turn, void *user)
{
    struct repl_loop_ctx *ctx = user;
    struct agent_state *state = ctx->state;
    struct agent_session *session = state->sess;
    const struct provider *provider = state->provider;
    struct session_stats *stats = &state->stats;
    const struct stream_usage *usage = &loop_turn->usage;

    stats->requests++;
    if (usage->input_tokens >= 0 && usage->output_tokens >= 0) {
        stats->last_ctx = usage->input_tokens + usage->output_tokens;
        stats->last_limit = model_meta_context(provider, session->model);
    }
    stats_account_usage(stats, usage, provider, session->model);
}

static int repl_loop_checkpoint(void *user)
{
    (void)user;
    interrupt_settle();
    if (interrupt_requested())
        return AGENT_LOOP_SIG_ABORT;
    if (interrupt_soft_requested())
        return AGENT_LOOP_SIG_PAUSE;
    return AGENT_LOOP_SIG_NONE;
}

static void repl_loop_tool_seen(const struct item *call, void *user)
{
    struct repl_loop_ctx *ctx = user;
    stats_count_tool_call(&ctx->state->stats, call->tool_name);
}

static struct item repl_loop_tool_call(const struct item *call, enum agent_loop_tool_action action,
                                       int image_input, void *user)
{
    struct repl_loop_ctx *ctx = user;
    struct render_ctx *r = ctx->state->r;
    if (action == AGENT_LOOP_TOOL_REFUSE) {
        render_transition(r, RS_IDLE);
        return dispatch_tool_refused(r, call);
    }
    if (action == AGENT_LOOP_TOOL_SKIP) {
        render_transition(r, RS_IDLE);
        return dispatch_tool_skipped(r, call);
    }
    return dispatch_tool_call(r, call, image_input);
}

static void repl_loop_compact(void *user)
{
    struct repl_loop_ctx *ctx = user;
    agent_compact(ctx->state, NULL, 1);
    /* agent_compact owns the watcher while it streams and leaves it disarmed.
     * A cancel — hard abort or soft pause — stays latched so the loop's
     * post-compact checkpoint sees it; otherwise restore the watcher for the
     * next continuation turn. */
    if (!interrupt_requested() && !interrupt_soft_requested()) {
        interrupt_clear();
        interrupt_arm();
    }
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

    /* Governs what this run *adds* to the two on-disk stores: the session
     * file opened below and the prompt history the editor appends to. Both
     * stay readable either way — --resume loads the file it was pointed at
     * and Up/Ctrl-R still recall earlier prompts; neither gains a line.
     * Re-resolved wherever a log is opened later (/resume), where a
     * /provider switch may since have moved the run off the dev backend that
     * suppressed this one; the history file is opened once, here, so it
     * follows the run's starting answer. */
    int recording = agent_recording_enabled(p);

    /* Resume: load a prior conversation into history before anything else
     * touches it, so the Ctrl-T view, the HAX_TRANSCRIPT mirror, and the
     * session log all see the restored items from the start. An unreadable
     * file is fatal — silently starting fresh would run against the wrong
     * history. An empty-but-readable session (e.g. truncated by a crash)
     * loads as zero items and just resumes empty, continuing that file. */
    size_t n_resumed = 0;
    /* The selection the resumed file records, kept until the session log is
     * opened against it below. main.c already restored it into the config, so
     * it differs from the live selection only when a flag overrode the
     * restore — which is then recorded as the switch it is. */
    struct session_meta rmeta;
    memset(&rmeta, 0, sizeof(rmeta));
    if (opts->resume_path) {
        struct item *loaded = NULL;
        size_t nl = 0;
        if (session_load(opts->resume_path, &loaded, &nl, &rmeta) != 0) {
            hax_err("could not resume session '%s'", opts->resume_path);
            session_meta_free(&rmeta);
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
    struct render_ctx r = {.disp = {.out = stdout, .trail = 1},
                           .show_reasoning = reasoning_visible()};
    r.spinner = spinner_new("working...");
    r.md = markdown_enabled() ? md_new(md_emit_to_disp, &r.disp, md_cols()) : NULL;
    /* On a --continue/--resume startup, replay the last user turn through
     * the live pipeline (same as /resume mid-session). Needs r/md ready,
     * so it lands here rather than right after the banner — nothing prints
     * in between, so it still reads as part of the startup sequence. */
    if (n_resumed > 0)
        replay_user_turn(&r, &sess, "resumed");
    struct input *input = input_new();
    /* Recall is a read either way — what `recording` decides is whether this
     * run's prompts join the file. */
    input_history_open_default(input, recording);
    input_set_modal_completer(input, &file_mention_completer);
    input_set_paste_cb(input, paste_cb, NULL);
    input_set_paste_filter(input, paste_filter_cb, NULL);
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
    /* The two paged conversation views, bound now that `state` — the
     * up-to-date session and provider they read through — exists. Ctrl-O is
     * safe to claim even though it is the tty's VDISCARD on BSD/macOS: the
     * editor's raw mode clears IEXTEN, so the driver never acts on it. */
    input_bind_modal_key(input, INPUT_KEY_CTRL('O'), show_history_cb, &state);
    input_bind_modal_key(input, INPUT_KEY_CTRL('T'), show_transcript_cb, &state);
    /* Append-only session record. Resuming continues the same file (so
     * the restored items aren't re-written); otherwise a fresh file is
     * begun. Left NULL when this run doesn't record — all entry points are
     * NULL-safe. */
    if (recording)
        state.slog = opts->resume_path
                         ? session_log_resume(opts->resume_path, rmeta.provider, rmeta.model,
                                              rmeta.effort, rmeta.preset, n_resumed)
                         : session_log_open(provider_log_name(p), sess.model, sess.effort,
                                            config_str("preset"));
    /* Stages the run's selection when it differs from what the file said —
     * i.e. when a selection flag redirected the resume — so the turns this
     * run produces are recorded under it and the next resume continues from
     * there. A no-op otherwise, and nothing reaches the file until a turn
     * does. */
    if (opts->resume_path)
        session_log_set_meta(state.slog, provider_log_name(p), sess.model, sess.effort,
                             config_str("preset"));
    session_meta_free(&rmeta);
    /* Mirror restored history into the HAX_TRANSCRIPT log — its header
     * was just written by _open; the items follow so the mirror matches
     * the live conversation. */
    if (n_resumed > 0)
        transcript_log_append(tlog, sess.items, sess.n_items);
    /* Initialize once — captures the canonical termios baseline and starts
     * the watcher thread. Idempotent; safe even when stdin/stdout aren't
     * ttys (becomes a no-op in that case). */
    interrupt_init();

    char prompt_buf[64];

    /* The render state in r (state + cluster sub-state) lives across
     * the continuation run so RS_CLUSTER can span consecutive silent tool
     * calls (read/grep/find...) without intervening blank lines.
     * End-of-user-turn cleanup unconditionally transitions back to
     * RS_IDLE, so leftover state from a prior user turn is impossible
     * by construction. */

    for (;;) {
        disp_block_separator(&r.disp);
        /* While a turn is resumable, the hint owns the empty-send meaning;
         * emitted with every prompt draw so it can't be scrolled away by
         * slash output while the state persists. */
        if (state.resume != AGENT_RESUME_NONE)
            render_resume_hint(&r, state.resume);
        /* Only a resumable turn gives an empty send a meaning; otherwise
         * the editor keeps swallowing bare Enter. */
        input_set_empty_submit(input, state.resume != AGENT_RESUME_NONE);
        cursor_show();
        /* Rebuilt each iteration so a runtime theme change (/config theme …)
         * recolors the prompt instead of keeping the startup theme's bytes. */
        char *line = input_readline(input, build_prompt(prompt_buf, sizeof(prompt_buf)));
        cursor_hide();
        if (!line) {
            putchar('\n');
            break;
        }
        /* An empty send is a no-op — except against a resumable turn,
         * where it means "continue": fall through with the empty line and
         * the resume path below re-enters the loop without new input.
         * Ctrl-C also returns "" but is a discard — a cancelled steering
         * draft must never launch the turn it was meant to redirect. */
        if (!*line && (state.resume == AGENT_RESUME_NONE || input_cancelled(input))) {
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
        if (*line) {
            struct slash_ctx sctx = {.state = &state};
            if (slash_dispatch(line, &sctx) != SLASH_NOT_A_COMMAND) {
                /* A /provider switch swaps state.provider (destroying the old
                 * one); resync the local so the next turn streams against the
                 * new provider and its context-limit / usage reads track it. */
                p = (struct provider *)state.provider;
                input_history_add_session(input, line);
                /* /undo and /fork stash the prompt they discarded; seed it after
                 * the command line so Up-arrow reaches it first. */
                if (state.pending_recall) {
                    input_history_add_session(input, state.pending_recall);
                    free(state.pending_recall);
                    state.pending_recall = NULL;
                }
                if (state.pending_preseed) {
                    input_set_preseed(input, state.pending_preseed);
                    free(state.pending_preseed);
                    state.pending_preseed = NULL;
                }
                free(line);
                continue;
            }

            input_history_add(input, line);
        }

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
         * response it produced read as one group. The shared runner inserts
         * subsequent boundaries before follow-up turns after tool dispatch.
         *
         * An empty send continues the pending resumable turn instead: a
         * marked stop (interrupt / mid-stream error) gets the CONTINUE_MARKER
         * user message so the transcript doesn't end on a bare stop, while a
         * clean seam gets nothing — the loop itself owes (and lazily appends)
         * the continued run's boundary. A typed message against a resumable
         * turn just steers — it is an ordinary user message and needs no
         * marker. */

        /* An Esc-touched or incomplete run skipped the end-of-turn
         * auto-compaction (Esc must return control without launching new
         * work), leaving compact_deferred set; the run that continues
         * settles the debt here, before its first request would exceed the
         * window — and before the user's input is appended, so a steering
         * message is never summarized away. */
        int compacted = 0;
        if (state.compact_deferred) {
            compacted = agent_compact(&state, NULL, 1);
            /* agent_compact cleared the latched interrupt flags on entry, so
             * any latch now is an Esc pressed during the transaction: the
             * user is backing out of the whole send, not just the
             * compaction, and the debt stands for the next attempt. Return
             * to the prompt — a typed steering line is already in editor
             * history, one Up-arrow away. */
            if (interrupt_requested() || interrupt_soft_requested()) {
                free(line);
                continue;
            }
            /* Settled on success (agent_compact clears the flag), attempted
             * on failure: either way, don't retry ahead of every send. */
            state.compact_deferred = 0;
        }

        /* After a successful compaction the summary seed replaced both the
         * paused turn and its markers: an empty send streams against the
         * seed as this run's user input, and like any continued run the
         * response still owes its lazily-appended boundary. */
        int continued = 0;
        /* /session's "user turns" counts typed prompts (fresh or steering);
         * an empty-send continuation — clean, [continue]-marked, or
         * compacted — extends the turn already counted, however many times
         * the loop pauses along the way. */
        int new_user_turn = *line != 0;
        if (*line)
            agent_session_add_user(&sess, line);
        else if (!compacted && state.resume_marked)
            agent_session_add_continuation(&sess);
        else
            continued = 1;
        free(line);
        /* input_readline left the cursor at column 0 of a fresh row. */
        r.disp.trail = 1;

        /* Startup probes the model's metadata in the background, so the
         * first prompts of a run can be the first moment this model's real
         * effort ladder is known. Say so when the pick moves: the banner
         * above is still asserting the level resolved without it, and the
         * one about to be sent is the true one. The session-log header is
         * likewise unwritten on the first turn, which is exactly when this
         * fires. */
        char *prev_effort = NULL;
        if (agent_session_resync_effort(&sess, p, &prev_effort)) {
            disp_block_separator(&r.disp);
            ui_note("effort %s → %s · %s", prev_effort ? prev_effort : "(unset)",
                    sess.effort ? sess.effort : "(unset)",
                    sess.model_label ? sess.model_label : "?");
            r.disp.trail = 1;
        }
        free(prev_effort);
        /* Re-stamp the header with the live selection whatever moved it —
         * this resync, /session's, or a compaction's. The header is still
         * unwritten on the first turn, which is when a late probe lands;
         * afterwards this is a no-op for the current file. */
        session_log_set_meta(state.slog, provider_log_name(p), sess.model, sess.effort,
                             config_str("preset"));
        /* Flush the prompt to the log immediately, before we hand
         * control to the provider. If the stream hangs or the process
         * is killed, the user prompt that triggered the in-flight call
         * is preserved on disk for post-mortem reading. */
        agent_loop_flush_logs(tlog, state.slog, sess.items, sess.n_items);

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

        /* ctx tracks the latest reported context size across this user
         * turn's model calls (= current window state, since each call's
         * input subsumes the prior call's prefix). -1 means "no call
         * reported it yet". */
        long user_turn_ctx = -1;
        long user_turn_start_ms = monotonic_ms();
        int user_turn_errored = 0;

        /* Immediate label reset: a fresh user turn is a clean slate,
         * and the previous turn's promoted label must not describe it
         * for a settle window (no flicker risk — the spinner was
         * hidden while the user typed). The timer uses the same clock
         * as the end-of-turn stats line so the two agree. */
        spinner_set_label(r.spinner, "working", "working...");
        spinner_set_timer(r.spinner, user_turn_start_ms);

        /* Arm the watcher for the duration of the continuation run — from
         * here on the first Esc requests a soft pause at the next loop
         * seam and a second aborts the stream or running tool. Cleared
         * first so a stray Esc from a previous user turn (e.g. user typed
         * Esc during readline editing) doesn't auto-cancel this one. */
        interrupt_clear();
        interrupt_arm();
        /* Optional per-user-turn round-trip budget ("check in with me every
         * N turns"): the loop stops AGENT_LOOP_MAX_TURNS at a clean seam and
         * the resumable prompt continues it on an empty send — a periodic
         * soft pause the agent applies to itself. 0/unset = unlimited. */
        int max_turns = config_int("max_turns");
        struct repl_loop_ctx loop_ctx = {.state = &state};
        struct agent_loop_params loop_params = {
            .session = &sess,
            .provider = p,
            .tlog = state.tlog,
            .slog = state.slog,
            .max_turns = max_turns > 0 ? max_turns : -1,
            .continued = continued,
            .hooks =
                {
                    .user = &loop_ctx,
                    .observe = repl_loop_on_event,
                    .tick = repl_loop_tick,
                    .turn_begin = repl_loop_turn_begin,
                    .turn_end = repl_loop_turn_end,
                    .checkpoint = repl_loop_checkpoint,
                    .tool_seen = repl_loop_tool_seen,
                    .tool_call = repl_loop_tool_call,
                    .compact = repl_loop_compact,
                },
        };
        struct agent_loop_result loop_result;
        agent_loop_run(&loop_params, &loop_result);
        user_turn_ctx = loop_result.last_context_tokens;
        user_turn_errored = loop_result.outcome == AGENT_LOOP_PROVIDER_ERROR;
        int user_turn_complete = loop_result.outcome == AGENT_LOOP_COMPLETE;
        /* Rederive the resumable state from this run's outcome: every
         * incomplete stop is continuable with an empty send, and only stops
         * whose repair left interrupt markers need the CONTINUE_MARKER
         * spoken on resume. */
        switch (loop_result.outcome) {
        case AGENT_LOOP_COMPLETE:
            resume_clear(&state);
            break;
        case AGENT_LOOP_PAUSED:
            state.resume = AGENT_RESUME_PAUSED;
            state.resume_marked = 0;
            break;
        case AGENT_LOOP_MAX_TURNS:
            state.resume = AGENT_RESUME_MAX_TURNS;
            state.resume_marked = 0;
            break;
        case AGENT_LOOP_INTERRUPTED:
            state.resume = AGENT_RESUME_INTERRUPTED;
            state.resume_marked = loop_result.abort_marker_placed;
            break;
        case AGENT_LOOP_PROVIDER_ERROR:
            state.resume = AGENT_RESUME_ERROR;
            state.resume_marked = loop_result.abort_marker_placed;
            break;
        }
        agent_loop_result_destroy(&loop_result);
        interrupt_disarm();
        /* Snapshot "did the user press Esc this run" now: the latched flags
         * survive disarm, but agent_compact (the auto path below) clears
         * them while it owns the watcher — reading lazily would forget the
         * keypress. Hard implies soft, so this covers every Esc. */
        int user_turn_soft = interrupt_soft_requested();
        /* The user turn is over: the end-of-user-turn auto-compaction
         * below and the next user turn's pre-stream spinner must not
         * carry this one's counter. */
        spinner_set_timer(r.spinner, 0);

        /* Close any open render state before post-user-turn output — a
         * still-running cluster spinner racing with notify_attention's
         * OSC-9 would corrupt the escape sequence. */
        render_transition(&r, RS_IDLE);

        /* No live [interrupted] marker line: the marker lives in history
         * (and replays from there), while the on-screen cue is the resume
         * hint the prompt loop is about to draw — rendering both would say
         * "interrupted" twice in three lines. */
        if (user_turn_complete && user_turn_soft) {
            /* A soft Esc raced the final response: the turn completed
             * before any pause point, so there is nothing to resume.
             * Say so — silence here reads as a dropped keypress. */
            render_open_block(&r);
            disp_raw(&r.disp, ANSI_DIM);
            disp_printf(&r.disp, "[finished before pause]");
            disp_raw(&r.disp, ANSI_RESET);
            disp_putc(&r.disp, '\n');
            disp_flush(&r.disp);
        }

        /* Time worked counts errored/interrupted turns too — the wall time
         * was spent either way, and /session's total should reflect it. */
        long user_turn_ms = monotonic_ms() - user_turn_start_ms;
        state.stats.worked_ms += user_turn_ms;
        if (new_user_turn)
            state.stats.turns++;

        if (!user_turn_errored)
            display_stats_line(&r, p, sess.model, user_turn_ctx, user_turn_ms, &state.stats);

        /* Auto-compaction: once the reported context usage nears the window,
         * summarize and replace history before the next prompt. Only after a
         * completed, Esc-free user turn: an errored/interrupted one has no
         * reliable token count and the user may want to retry against intact
         * history; a resumable stop — or any Esc, including one that lost
         * the race to the final response — must not launch another long
         * model request right after the user asked to stop. Clean stops
         * (pause, max turns, a raced Esc on a completed turn) record the
         * debt instead, and the next send's preflight settles it. Errored
         * and interrupted turns record nothing: their partial/marked state
         * is exactly what a retry or [continue] must present intact, and if
         * overflow is what broke the turn, the error is the user's cue to
         * /compact explicitly. Runs at this natural pause — the model has
         * finished responding and we're about to wait for input — so no
         * mid-task continuation is needed. */
        if (compact_should_auto(user_turn_ctx, model_meta_context(p, sess.model))) {
            if (user_turn_complete && !user_turn_soft)
                agent_compact(&state, NULL, 1);
            else if (user_turn_complete || state.resume == AGENT_RESUME_PAUSED ||
                     state.resume == AGENT_RESUME_MAX_TURNS)
                state.compact_deferred = 1;
        }

        /* Ping the terminal so the user gets a notification / dock
         * bounce when hax is back to idle. Skipped after any Esc this
         * run — pause, hard interrupt, or a pause that lost the race to
         * the final response — since the user just pressed a key:
         * they're already at the terminal. Errored and max-turns user
         * turns still notify: the user needs to know the request
         * bounced / the loop is waiting on them. */
        if (!user_turn_soft)
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
    spend_free(&state.stats.spend);
    free(state.pending_preseed);
    agent_session_free(&sess);
    return 0;
}
