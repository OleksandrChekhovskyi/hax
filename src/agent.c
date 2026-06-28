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
#include "compact.h"
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

/* Result of absorbing an aborted turn. had_state means some provider content
 * arrived; marker_placed means [interrupted] landed in text or a tool_result. */
struct absorb_outcome {
    int had_state;
    int marker_placed;
};

/* Normalize aborted state for history: tag partial text and synthesize
 * [interrupted] results for unrun calls. The outcome lets EV_ERROR skip empty
 * pre-stream failures while Esc still leaves a visible marker. */
static struct absorb_outcome absorb_aborted_turn(struct agent_session *sess, struct turn *t)
{
    /* Snapshot partial state before flushing. Reasoning is flushed before text
     * to preserve normal item order instead of being lost in turn_reset. */
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

/* True when a tool_result already carries bash's Esc-kill marker; other tools
 * can finish cleanly and still need a synthetic abort marker afterward. */
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

/* Record a user abort unless the last tool_result already carries the marker. */
static void append_interrupt_marker(struct agent_session *s)
{
    if (s->n_items == 0 || !tool_result_is_marked(&s->items[s->n_items - 1]))
        items_append(
            &s->items, &s->n_items, &s->cap_items,
            (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup(INTERRUPT_MARKER)});
}

/* Advance transcript and session logs together; both appenders are NULL/no-op safe. */
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

/* Delay before the non-disruptive inline "still alive" glyph appears. */
#define TEXT_IDLE_TIMEOUT_MS 1500

/* Defer naming the composing tool long enough to avoid fast-call label flicker. */
#define TOOL_COMPOSE_LABEL_MS 500

/* Let markdown emit content through disp while raw ANSI escapes bypass disp's
 * newline bookkeeping; zero-width escapes must not commit held newlines. */
static void md_emit_to_disp(const char *bytes, size_t n, int is_raw, void *user)
{
    if (is_raw)
        fwrite(bytes, 1, n, stdout);
    else
        disp_write((struct disp *)user, bytes, n);
}

static int markdown_enabled(void)
{
    /* Markdown uses cursor-control escapes, so piped stdout is always raw;
     * on TTYs, the config key is only an off-switch. */
    if (!isatty(fileno(stdout)))
        return 0;
    return config_bool("markdown");
}

/* Reserve the physical last column for markdown wrapping. The retro-wrap
 * engine assumes the cursor moves past the last glyph; terminals with deferred
 * autowrap keep it on the last column, making CSI back/erase eat a character. */
static int md_wrap_width(void)
{
    int w = display_width();
    int edge = term_width() - 1;
    return w < edge ? w : edge;
}

/* Draw received reasoning deltas live; backend opt-in is handled elsewhere. */
static int reasoning_visible(void)
{
    return config_bool("show_reasoning");
}

/* Format tokens in 1024-base (32k/128k context windows stay round); <0 -> "?". */
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

/* Per-user-turn usage summary. ctx/cached use the last response (current
 * window); out is summed across model calls. Unreported values stay hidden. */
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
        long limit = compact_context_limit(p);
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

/* Per-stream side-channel: update idle/retry UI and return Esc state. Sticky
 * inline idle glyphs disappear under resumed text without touching disp. */
static int agent_stream_tick(void *user)
{
    struct render_ctx *r = user;
    /* Retry countdown wins over idle UI; repaint each tick as seconds shrink. */
    if (r->retry_deadline_at) {
        update_retry_label(r);
    } else if (r->compose_at && monotonic_ms() - r->compose_at >= TOOL_COMPOSE_LABEL_MS) {
        /* Name the long-running compose once; keep back-to-back batches labeled. */
        spinner_set_label(r->spinner, r->compose_label);
        r->compose_at = 0;
        r->composing_active = 1;
    } else if (r->compose_end_at && monotonic_ms() - r->compose_end_at >= TOOL_COMPOSE_LABEL_MS) {
        /* Args ended and nothing followed; stop claiming we're composing. */
        spinner_set_label(r->spinner, "working...");
        r->composing_active = 0;
        r->compose_end_at = 0;
    } else if (r->md && md_in_table(r->md)) {
        /* Tables buffer until laid out; show a line spinner for mid-table stalls. */
        if (!r->table_composing && r->last_text_at &&
            monotonic_ms() - r->last_text_at >= TEXT_IDLE_TIMEOUT_MS)
            render_table_spinner_show(r);
    } else if (r->last_text_at && !r->idle_shown &&
               monotonic_ms() - r->last_text_at >= TEXT_IDLE_TIMEOUT_MS) {
        /* Skip unsafe inline glyphs: held newlines or last-column autowrap make
         * backspace erase unreliable. Latch idle_shown to avoid churn. */
        if (r->md && r->disp.held == 0) {
            /* Avoid a temporary double-space when the stream already left padding. */
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

    /* Track idle since the last visible text byte; seams switch long pauses to
     * the full busy spinner instead of the inline glyph. */
    if (ev->kind == EV_DONE || ev->kind == EV_ERROR)
        r->last_text_at = 0;
    /* Any event other than EV_RETRY itself means we're past the
     * backoff sleep — clear the countdown so per-event label updates
     * aren't fighting the tick. */
    if (ev->kind != EV_RETRY && r->retry_deadline_at) {
        r->retry_deadline_at = 0;
        spinner_set_label(r->spinner, "working...");
    }
    /* Only arg deltas keep a deferred "composing..." swap alive; START re-arms. */
    if (ev->kind != EV_TOOL_CALL_DELTA)
        r->compose_at = 0;
    /* Cancel post-END label reverts when the next event proves the batch continued. */
    if (ev->kind != EV_TOOL_CALL_DELTA && ev->kind != EV_TOOL_CALL_END)
        r->compose_end_at = 0;

    switch (ev->kind) {
    case EV_TEXT_DELTA:
        /* Strip the first delta's leading newlines, open RS_TEXT, feed —
         * shared with resume replay so both render identically. */
        render_text_delta(r, ev->u.text_delta.text, strlen(ev->u.text_delta.text));
        break;
    case EV_TOOL_CALL_START: {
        /* Defer compose labels to avoid flicker; provider interleaving can only
         * mislabel the spinner briefly, and dispatch prints real headers. */
        const char *name = ev->u.tool_call_start.name;
        if (name && *name)
            snprintf(r->compose_label, sizeof(r->compose_label), "[%s] composing...", name);
        if (r->composing_active && name && *name && r->state == RS_WAITING) {
            /* Back-to-back call: switch labels directly instead of flashing "working...". */
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
        /* Revert a composing label only if the model stalls after finalized args. */
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
        /* Stash retry deadline for live countdown; attempt+1 is next attempt. */
        r->retry_deadline_at = monotonic_ms() + ev->u.retry.delay_ms;
        r->retry_attempt = ev->u.retry.attempt + 1;
        r->retry_max = ev->u.retry.max_attempts;
        update_retry_label(r);
        break;
    }
    case EV_PROGRESS: {
        /* Progress excludes cached tokens so each turn starts at 0%; first
         * content/reasoning delta overwrites this spinner label. */
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

/* Hide the cursor except while reading input. Require both stdin/stdout TTYs
 * to match interrupt_init(); otherwise no abnormal-exit restore is installed. */
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

/* Ctrl-T view points at live session fields because items/n_items move as the
 * vector grows. Lives on agent_run's stack; input_free must unregister it. */
struct transcript_view {
    const char *const *sys_ref;
    struct item *const *items_ref;
    const size_t *n_items_ref;
    /* Tools are stable for the session, so no indirection needed. */
    const struct tool_def *tools;
    size_t n_tools;
};

static void show_transcript_cb(void *user)
{
    struct transcript_view *v = user;
    const char *pager = getenv("PAGER");
    if (!pager || !*pager)
        pager = "less -R";
    /* Isolate pager SIGINT/SIGQUIT/SIGPIPE from hax; child keeps defaults. */
    struct spawn_pipe sp;
    if (spawn_pipe_open(&sp, pager) < 0)
        return;
    transcript_render(sp.w, *v->sys_ref, v->tools, v->n_tools, *v->items_ref, *v->n_items_ref);
    spawn_pipe_close(&sp);
}

/* Print just the two banner rows; callers own surrounding blank-line spacing. */
void agent_print_banner(const struct provider *p, const struct agent_session *s)
{
    const char *bar = ANSI_CYAN "▌" ANSI_FG_DEFAULT;
    if (!p) {
        /* Start provider-less so the user can choose a working one. */
        printf("%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM
               "› no provider — use /provider" ANSI_BOLD_OFF "\n",
               bar);
        printf("%s " ANSI_DIM "ctrl-d quit · try /help" ANSI_BOLD_OFF "\n", bar);
        return;
    }
    const char *name = p->name ? p->name : "?";
    if (!s->model || !*s->model)
        /* Start model-less so the user can choose one. */
        printf("%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM
               "› %s · no model — use /model" ANSI_BOLD_OFF "\n",
               bar, name);
    else if (s->reasoning_effort)
        printf("%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM "› %s · %s · %s" ANSI_BOLD_OFF "\n",
               bar, name, s->model, s->reasoning_effort);
    else
        printf("%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM "› %s · %s" ANSI_BOLD_OFF "\n", bar,
               name, s->model);
    printf("%s " ANSI_DIM "ctrl-d quit · try /help" ANSI_BOLD_OFF "\n", bar);
}

/* Provider name for logs, tolerating provider-less interactive startup. */
static const char *provider_log_name(const struct provider *p)
{
    return p && p->name ? p->name : "none";
}

void agent_set_provider(struct agent_state *st, struct provider *newp)
{
    struct provider *old = (struct provider *)st->provider;
    st->provider = newp;
    /* Destroy after the swap so observers never see a half-replaced provider. */
    if (old)
        old->destroy(old);
}

int agent_apply_settings(struct agent_state *st)
{
    struct agent_session *s = st->sess;
    struct provider *p = (struct provider *)st->provider;
    /* Compare after reconfigure to distinguish real model changes from /effort. */
    char *prev_model = s->model ? xstrdup(s->model) : NULL;
    if (agent_session_reconfigure(s, p) != 0) {
        free(prev_model);
        return -1;
    }

    /* Re-probe context limits only when the resolved model actually changed. */
    int model_changed = (prev_model == NULL) != (s->model == NULL) ||
                        (prev_model && s->model && strcmp(prev_model, s->model) != 0);
    free(prev_model);
    if (model_changed && p && p->refresh_context)
        p->refresh_context(p, s->model);

    /* Re-key HAX_TRANSCRIPT to the rebuilt prompt so it matches Ctrl-T after a
     * settings change. No-op when HAX_TRANSCRIPT is unset. */
    transcript_log_reset(st->tlog, s->sys, s->tools, s->n_tools);
    transcript_log_append(st->tlog, s->items, s->n_items);

    /* Update the lazy session-log header before first append; later calls are
     * no-ops for this file, but seed the metadata used after /new. */
    session_log_set_meta(st->slog, provider_log_name(p), s->model, s->reasoning_effort);

    /* Screen-only UI hint; don't add settings changes to model history/logs. */
    char *label = s->reasoning_effort
                      ? xasprintf("switched to %s · %s · %s", p->name ? p->name : "?", s->model,
                                  s->reasoning_effort)
                      : xasprintf("switched to %s · %s", p->name ? p->name : "?", s->model);

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
    /* Cleared conversations start a fresh session record. */
    session_log_reset(st->slog);
    /* Old bash temp files are unreachable once their referring turns are gone. */
    bash_cleanup_tempfiles();
    agent_print_banner(st->provider, st->sess);
}

/* Render a stored/live standalone interrupt marker as a dim out-of-band block. */
static void render_interrupt_marker(struct render_ctx *r)
{
    render_open_block(r);
    disp_raw(ANSI_DIM);
    disp_printf(&r->disp, "%s", INTERRUPT_MARKER);
    disp_raw(ANSI_RESET);
    disp_putc(&r->disp, '\n');
    fflush(stdout);
}

/* Replay a stored user prompt through the same renderer as live input, then
 * resync disp because the input renderer writes directly to stdout. */
static void replay_user_echo(struct render_ctx *r, const char *text)
{
    render_open_block(r);
    /* Match the editor's wrap width so replayed prompts wrap like live input. */
    input_render_user_message(text ? text : "", text ? strlen(text) : 0, input_display_cols());
    r->disp.trail = 1;
    r->disp.held = 0;
    r->disp.at_space_or_bol = 1;
}

/* Replay stored text through the live markdown path. Empty/opaque reasoning is
 * a no-op, matching live display. */
static void replay_text(struct render_ctx *r, enum render_state target, const char *text)
{
    if (!text || !*text)
        return;
    if (r->state != target) {
        /* Flush the old markdown stream before reset; resetting first would drop
         * deferred bytes. Same-kind items stay in one stream. */
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

/* Replay assistant text, splitting a stored line-boundary interrupt marker into
 * the same dim block the live path uses. */
static void replay_assistant(struct render_ctx *r, const char *text)
{
    if (!text || !*text)
        return;
    size_t len = strlen(text);
    size_t mlen = strlen(INTERRUPT_MARKER);
    if (len >= mlen && strcmp(text + len - mlen, INTERRUPT_MARKER) == 0) {
        size_t before = len - mlen;
        if (before == 0 || text[before - 1] == '\n') {
            if (before > 1) {
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

/* Replay the last user prompt and its model/tool follow-ups on interactive
 * resume, leaving earlier history to Ctrl-T. Tool output previews can't be
 * rebuilt safely, so calls collapse to headers. */
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

    /* Banner/picker bypass disp; resync held/space state and trust caller-set
     * trail for the separator. */
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

    /* End replay at column 0 with one committed newline so the next prompt has
     * normal spacing. */
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
        /* Error printed one fresh row; correct trail for the next prompt. */
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

    /* Continue the resumed session file and re-key HAX_TRANSCRIPT to it. */
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

/* Summary stream sink: feed turn.c and capture errors, but render nothing. */
struct compact_ev {
    struct turn *turn;
    char *err; /* owned; set from EV_ERROR */
};

static int compact_on_event(const struct stream_event *ev, void *user)
{
    struct compact_ev *ce = user;
    if (ev->kind == EV_ERROR && !ce->err)
        ce->err = xstrdup(ev->u.error.message ? ev->u.error.message : "stream failed");
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
        /* Only manual /compact can reach this; auto needs a provider-backed turn. */
        if (!is_auto)
            compact_notice(r, "no provider selected — use /provider");
        return 0;
    }
    if (!s->model || !*s->model) {
        /* Manual /compact can reach this on resumed history; auto cannot stream. */
        if (!is_auto)
            compact_notice(r, "no model selected — use /model (or /provider)");
        return 0;
    }
    if (s->n_items == 0) {
        if (!is_auto)
            compact_notice(r, "nothing to compact");
        return 0;
    }

    /* Close caller state so the compaction spinner starts on a clean line. */
    render_transition(r, RS_IDLE);

    /* Spinner-only summary request; compact_on_event feeds turn.c and errors. */
    render_stream_begin(r);
    spinner_set_label(r->spinner, "compacting...");

    struct turn t;
    turn_init(&t);
    struct compact_ev ce = {.turn = &t};

    interrupt_clear();
    interrupt_arm();
    char *summary =
        compact_summarize(s, p, instructions, &t, compact_on_event, &ce, agent_stream_tick, r);
    interrupt_settle();
    int cancelled = interrupt_requested();
    interrupt_disarm();

    render_transition(r, RS_IDLE);

    /* Cancel can return a partial summary; discard it. */
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
    /* `p` tracks the live provider across /provider swaps and is returned to caller. */
    struct provider *p = *provider;
    struct agent_session sess;
    if (agent_session_init(&sess, p, opts) < 0)
        return 1;

    /* Load resumed history before logs/Ctrl-T are opened; unreadable sessions are
     * fatal so the run cannot silently use the wrong context. */
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
    /* Render state shared by all render calls; spinner/md handles are owned here. */
    struct render_ctx r = {.disp = {.trail = 1, .at_space_or_bol = 1},
                           .show_reasoning = reasoning_visible()};
    r.spinner = spinner_new("working...");
    r.md = markdown_enabled() ? md_new(md_emit_to_disp, &r.disp, md_wrap_width()) : NULL;
    /* Replay after render setup so resumed output uses the live display path. */
    if (n_resumed > 0)
        replay_user_turn(&r, &sess);
    struct input *input = input_new();
    input_history_open_default(input);
    struct transcript_view tv = {
        /* Point at live session fields; xrealloc can move the items vector. */
        .sys_ref = (const char *const *)&sess.sys,
        .items_ref = &sess.items,
        .n_items_ref = &sess.n_items,
        .tools = sess.tools,
        .n_tools = sess.n_tools,
    };
    input_set_transcript_cb(input, show_transcript_cb, &tv);
    /* HAX_TRANSCRIPT mirror; NULL-safe when unset. */
    struct transcript_log *tlog = transcript_log_open(sess.sys, sess.tools, sess.n_tools);
    /* Slash handlers borrow this stack-owned aggregate; /resume may swap slog. */
    struct agent_state state = {.sess = &sess, .provider = p, .tlog = tlog, .r = &r};
    /* Session log: resume the existing file or start a fresh NULL-safe one. */
    state.slog = opts->resume_path
                     ? session_log_resume(opts->resume_path, provider_log_name(p), sess.model,
                                          sess.reasoning_effort, n_resumed)
                     : session_log_open(provider_log_name(p), sess.model, sess.reasoning_effort);
    /* Mirror restored history after transcript header creation. */
    if (n_resumed > 0)
        transcript_log_append(tlog, sess.items, sess.n_items);
    /* Capture terminal baseline and start the interrupt watcher (TTY-gated). */
    interrupt_init();

    const char *prompt = locale_have_utf8() ? PROMPT_UTF8 : PROMPT_ASCII;

    /* Keep render state across inner-loop tool batches so silent calls can share
     * one cluster; user-turn cleanup returns it to idle. */

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

        /* Slash commands are local and session-recallable, but not persisted;
         * non-command slash-looking paths fall through to the model. */
        /* Slash output bypasses disp; set the common post-command trail before
         * dispatch so unusual handlers can override it. */
        r.disp.trail = 1;
        struct slash_ctx sctx = {.state = &state};
        if (slash_dispatch(line, &sctx) != SLASH_NOT_A_COMMAND) {
            /* /provider may swap/destroy state.provider; resync local `p`. */
            p = (struct provider *)state.provider;
            input_history_add_session(input, line);
            free(line);
            continue;
        }

        input_history_add(input, line);

        /* No provider: keep the prompt recallable and point the user at /provider. */
        if (!p) {
            /* Emit the same leading gap a model turn or slash note gets. */
            disp_block_separator(&r.disp);
            ui_note("no provider selected — use /provider to choose one, then resend");
            r.disp.trail = 1;
            free(line);
            continue;
        }

        /* No model: keep the prompt recallable and point at runtime selectors. */
        if (!sess.model || !*sess.model) {
            disp_block_separator(&r.disp); /* leading gap, as above */
            ui_note("no model selected — use /model (or /provider) to choose one, then resend");
            r.disp.trail = 1;
            free(line);
            continue;
        }

        /* Put TURN_BOUNDARY before the user message so transcript rules group the
         * prompt with the response it triggered; tool follow-ups add their own. */
        agent_session_add_user(&sess, line);
        free(line);
        /* Log the prompt before streaming so hangs/crashes keep it visible. */
        flush_logs(tlog, state.slog, sess.items, sess.n_items);
        /* input_readline left the cursor at column 0 of a fresh row. */
        r.disp.trail = 1;

        /* Usage summary state for this user turn: latest ctx/cache, summed output. */
        long user_turn_ctx = -1, user_turn_out = -1, user_turn_cached = -1;
        int user_turn_errored = 0;
        int user_turn_interrupted = 0;

        /* Esc now aborts the stream/tool; clear stale Esc from readline first. */
        interrupt_clear();
        interrupt_arm();
        /* Keep awake across streaming/tool dispatch; released before human input. */
        keepawake_acquire();
        /* First stream uses the user-message boundary; follow-ups add one here. */
        int first_inner = 1;
        for (;;) {
            if (!first_inner) {
                /* Add before ctx is built so xrealloc cannot dangle ctx.items. */
                agent_session_add_boundary(&sess);
                /* Log the boundary before waiting on the next stream. */
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
            /* Tick handles Esc and idle UI from curl progress/chunk callbacks. */
            p->stream(p, &ctx, sess.model, on_event, &ec, agent_stream_tick, &r);

            if (t.error) {
                /* Preserve partial assistant state after stream errors; on_event
                 * already drew the visible error line. */
                struct absorb_outcome o = absorb_aborted_turn(&sess, &t);
                if (o.had_state && !o.marker_placed) {
                    /* If content arrived but no absorbed item carried the marker,
                     * add one; skip pre-stream failures to avoid fake assistant turns. */
                    items_append(&sess.items, &sess.n_items, &sess.cap_items,
                                 (struct item){
                                     .kind = ITEM_ASSISTANT_MESSAGE,
                                     .text = xstrdup(INTERRUPT_MARKER),
                                 });
                }
                /* Error exits skip the success flush; persist partial history now. */
                flush_logs(tlog, state.slog, sess.items, sess.n_items);
                user_turn_errored = 1;
                break;
            }

            /* Let a just-pressed Esc leave the classifier window before dispatch. */
            interrupt_settle();
            int interrupted = interrupt_requested();

            if (ec.usage.input_tokens >= 0 && ec.usage.output_tokens >= 0)
                user_turn_ctx = ec.usage.input_tokens + ec.usage.output_tokens;
            if (ec.usage.output_tokens >= 0)
                user_turn_out = (user_turn_out < 0 ? 0 : user_turn_out) + ec.usage.output_tokens;
            if (ec.usage.cached_tokens >= 0)
                user_turn_cached = ec.usage.cached_tokens;

            if (interrupted) {
                /* Esc uses aborted-turn cleanup and always leaves a visible marker,
                 * even if no deltas arrived. */
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

            /* Normal completion: absorb and decide whether tools require another turn. */
            size_t n_before;
            int had_tool_call;
            agent_session_absorb(&sess, &t, &n_before, &had_tool_call);
            turn_reset(&t);

            if (!had_tool_call)
                break;

            /* Dispatch tool calls as non-interleaved blocks; Esc converts remaining
             * calls to [interrupted] results so history stays well-formed. */
            size_t current_end = sess.n_items;
            for (size_t i = n_before; i < current_end; i++) {
                if (sess.items[i].kind != ITEM_TOOL_CALL)
                    continue;
                interrupt_settle();
                struct item result;
                if (sess.n_tools == 0) {
                    /* --raw disables local execution even if the backend returns calls. */
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

            /* Flush completed assistant/tool/result slices before any follow-up stream
             * so crashes preserve a well-formed transcript/log. */
            flush_logs(tlog, state.slog, sess.items, sess.n_items);

            /* Esc during/after tools stops the loop and ensures history carries a marker. */
            interrupt_settle();
            if (interrupt_requested()) {
                append_interrupt_marker(&sess);
                user_turn_interrupted = 1;
                break;
            }

            /* Compact before the next autonomous round-trip when the completed tool
             * step already nears the window; end-of-turn compaction handles idle time. */
            if (compact_should_auto(user_turn_ctx, compact_context_limit(p))) {
                agent_compact(&state, NULL, 1);
                /* Esc during compaction means stop rather than continue uncompacted. */
                if (interrupt_requested()) {
                    append_interrupt_marker(&sess);
                    user_turn_interrupted = 1;
                    break;
                }
                /* agent_compact disarms the watcher; re-arm for remaining turns. */
                interrupt_clear();
                interrupt_arm();
            }
        }
        interrupt_disarm();
        keepawake_release();

        /* Close render state before post-turn usage/notifications. */
        render_transition(&r, RS_IDLE);

        /* Catch no-tool exits and interrupt paths; appender no-ops if already current. */
        flush_logs(tlog, state.slog, sess.items, sess.n_items);

        if (user_turn_interrupted)
            render_interrupt_marker(&r);

        if (!user_turn_errored)
            display_usage(&r, p, user_turn_ctx, user_turn_out, user_turn_cached);

        /* At the idle boundary, compact before accepting the next user prompt. Skip
         * errored/interrupted turns to preserve retryable history. */
        if (!user_turn_errored && !user_turn_interrupted &&
            compact_should_auto(user_turn_ctx, compact_context_limit(p)))
            agent_compact(&state, NULL, 1);

        /* Notify when back idle, except after Esc because the user is already present. */
        if (!user_turn_interrupted)
            notify_attention();
    }

    /* Print a resume hint for non-empty, persistent sessions. */
    const char *hint = session_log_resume_hint(state.slog);
    if (hint)
        ui_note("resume with: hax --resume=%s", hint);

    /* Return the live provider so the caller destroys the current one. */
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
