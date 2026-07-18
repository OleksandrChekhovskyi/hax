/* SPDX-License-Identifier: MIT */
#include "oneshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "agent_loop.h"
#include "catalog.h"
#include "compact.h"
#include "config.h"
#include "session.h"
#include "transcript.h"
#include "util.h"
#include "terminal/ansi.h"

/* Compaction attempts count toward the one-shot exit spend just like normal
 * turns. History mutation and footer capture stay inside compact_run. */
struct oneshot_compact_ctx {
    struct spend_totals *costs;
    const char *catalog_id;
    const char *model;
};

static int compact_on_event(const struct stream_event *ev, void *user)
{
    struct oneshot_compact_ctx *ctx = user;
    const struct stream_usage *usage = NULL;
    if (ev->kind == EV_DONE)
        usage = &ev->u.done.usage;
    else if (ev->kind == EV_ERROR)
        usage = ev->u.error.usage;
    if (usage)
        spend_account(ctx->costs, usage, ctx->catalog_id, ctx->model);
    return 0;
}

/* Walk `items` and write the text of every assistant message produced
 * after `from` to stdout, one terminating newline per message. The model
 * may emit multiple assistant messages in a single response (rare with
 * the providers we ship, but legal); printing all of them avoids
 * silently swallowing content. */
static void print_assistant_text(const struct item *items, size_t from, size_t to)
{
    for (size_t i = from; i < to; i++) {
        if (items[i].kind != ITEM_ASSISTANT_MESSAGE || !items[i].text)
            continue;
        size_t n = strlen(items[i].text);
        if (n == 0)
            continue;
        fwrite(items[i].text, 1, n, stdout);
        if (items[i].text[n - 1] != '\n')
            fputc('\n', stdout);
    }
}

static int oneshot_compact(struct agent_session *session, struct provider *provider,
                           struct session_log *slog, struct transcript_log *tlog,
                           struct spend_totals *costs)
{
    struct oneshot_compact_ctx ctx = {
        .costs = costs,
        .catalog_id = provider->catalog_id,
        .model = session->model,
    };
    struct compact_params params = {
        .session = session,
        .provider = provider,
        .slog = slog,
        .tlog = tlog,
        .hooks = {.user = &ctx, .observe = compact_on_event},
    };
    struct compact_result result;
    compact_run(&params, &result);
    int compacted = result.outcome == COMPACT_COMPLETE;
    compact_result_destroy(&result);
    return compacted;
}

struct oneshot_loop_ctx {
    struct agent_session *session;
    struct provider *provider;
    struct session_log *slog;
    struct transcript_log *tlog;
    struct spend_totals *costs;
};

static void oneshot_turn_end(const struct agent_loop_turn *loop_turn, void *user)
{
    struct oneshot_loop_ctx *ctx = user;
    spend_account(ctx->costs, &loop_turn->usage, ctx->provider->catalog_id, ctx->session->model);
}

static void oneshot_auto_compact(void *user)
{
    struct oneshot_loop_ctx *ctx = user;
    if (!oneshot_compact(ctx->session, ctx->provider, ctx->slog, ctx->tlog, ctx->costs))
        return;
    int tty = isatty(fileno(stderr));
    fprintf(stderr, "%s[compacted context]%s\n", tty ? ANSI_DIM : "", tty ? ANSI_RESET : "");
}

int oneshot_run(struct provider *p, const char *prompt, const struct hax_opts *opts, int max_turns)
{
    struct agent_session sess;
    if (agent_session_init(&sess, p, opts) < 0)
        return 1;
    /* No interactive picker in -p mode, so a missing model is fatal here
     * (agent_session_init now tolerates it for the REPL's sake). */
    if (!sess.model || !*sess.model) {
        hax_err("HAX_MODEL is required for provider '%s' (no default)", p->name ? p->name : "?");
        agent_session_free(&sess);
        return 1;
    }

    /* Resume: seed history from a prior session before the new prompt is
     * added, so -p can continue a conversation. An unreadable file is fatal
     * rather than silently running the prompt against empty history; an
     * empty-but-readable session loads as zero items and resumes empty. */
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

    /* HAX_TRANSCRIPT mirror, opened (and the file truncated) before
     * any model call so even an early-error or zero-turn run leaves
     * a fresh file behind. NULL when the env var is unset; all
     * transcript_log_* entry points are NULL-safe. Appended at the end
     * of each round-trip so `tail -f` works during long agentic runs;
     * the final append during cleanup catches any remaining items and
     * is idempotent on the others (transcript_log_append no-ops when
     * n_items hasn't grown). */
    struct transcript_log *tlog = transcript_log_open(sess.sys, sess.tools, sess.n_tools);
    /* Append-only session record — continue the resumed file, else begin
     * a fresh one. NULL when persistence is disabled. */
    struct session_log *slog =
        opts->resume_path
            ? session_log_resume(opts->resume_path, p->name, sess.model, sess.effort, n_resumed)
            : session_log_open(p->name, sess.model, sess.effort);
    if (n_resumed > 0)
        transcript_log_append(tlog, sess.items, sess.n_items);

    agent_session_add_user(&sess, prompt);
    /* Land the prompt on disk before any provider call so a hang or
     * crash mid-stream still leaves the triggering input visible in
     * the log. */
    agent_loop_flush_logs(tlog, slog, sess.items, sess.n_items);

    /* Provenance banner — the light -p twin of the REPL's agent_print_banner.
     * What actually answers resolves through several tiers (env, state.json,
     * config, auto-selection), so name it up front, before any model call.
     * stderr keeps piped stdout clean; deliberately not TTY-gated so CI and
     * pipeline logs capture which backend produced the answer. Dim only on a
     * terminal, like the resume hint below. Emitted after the first flush
     * (not at entry) so the session id can ride along: the flush above
     * materialized the file, and an id visible *at startup* is what lets a
     * subagent killed mid-run (bash-tool timeout) still be picked up with
     * --resume — the exit-time hint never prints for it. */
    {
        int tty = isatty(fileno(stderr));
        const char *model_label = sess.model_label ? sess.model_label : sess.model;
        /* Active preset stance, like the REPL banner: the answer's
         * provenance includes the stance that shaped it (possibly a swapped
         * system prompt), not just the backend. */
        const char *preset = config_str("preset");
        if (preset && *preset)
            fprintf(stderr, "%shax [%s]: %s · %s", tty ? ANSI_DIM : "", preset,
                    p->name ? p->name : "?", model_label);
        else
            fprintf(stderr, "%shax: %s · %s", tty ? ANSI_DIM : "", p->name ? p->name : "?",
                    model_label);
        if (sess.effort)
            fprintf(stderr, " · %s", sess.effort);
        if (opts->provider_autoselected)
            fprintf(stderr, " (auto-selected)");
        const char *sid = session_log_resume_hint(slog);
        if (sid)
            fprintf(stderr, " · session %s", sid);
        /* Trailing blank line for the same reason the resume hint leads with
         * one: in a combined 2>&1 stream the banner would otherwise run
         * straight into the answer. */
        fprintf(stderr, "%s\n\n", tty ? ANSI_RESET : "");
    }

    int rc = 0;
    /* Run stats for the exit summary on stderr: latest reported context
     * size, summed provider-reported cost (compaction turns included, via
     * oneshot_compact's cost accumulator), wall time for the whole run. */
    long start_ms = monotonic_ms();
    long last_ctx = -1;
    struct spend_totals costs = {0};
    /* A run on a catalog-mapped provider will want metadata for the exit
     * estimate / window fallback — start the background refresh now so it
     * lands while the model works. A snapshot that has been failing to
     * refresh for weeks earns a warning: estimates may have drifted. */
    if (p->catalog_id) {
        long stale_days = catalog_prefetch();
        if (stale_days > 0)
            hax_warn("model catalog last refreshed %ld days ago — cost estimates may be stale",
                     stale_days);
    }
    struct oneshot_loop_ctx loop_ctx = {
        .session = &sess,
        .provider = p,
        .slog = slog,
        .tlog = tlog,
        .costs = &costs,
    };
    struct agent_loop_params loop_params = {
        .session = &sess,
        .provider = p,
        .tlog = tlog,
        .slog = slog,
        .max_turns = max_turns,
        .hooks =
            {
                .user = &loop_ctx,
                .turn_end = oneshot_turn_end,
                .compact = oneshot_auto_compact,
            },
    };
    struct agent_loop_result loop_result;
    agent_loop_run(&loop_params, &loop_result);
    last_ctx = loop_result.last_context_tokens;

    switch (loop_result.outcome) {
    case AGENT_LOOP_COMPLETE:
        print_assistant_text(sess.items, loop_result.final_items_from, loop_result.final_items_to);
        break;
    case AGENT_LOOP_PROVIDER_ERROR:
        hax_err("provider error: %s",
                loop_result.error_message ? loop_result.error_message : "(no message)");
        rc = 1;
        break;
    case AGENT_LOOP_INTERRUPTED:
    case AGENT_LOOP_PAUSED: /* no checkpoint hook here — not reachable */
        rc = 1;
        break;
    case AGENT_LOOP_MAX_TURNS:
        hax_err("max turns (%d) exceeded; aborting", max_turns);
        rc = 1;
        break;
    }
    agent_loop_result_destroy(&loop_result);

    agent_loop_flush_logs(tlog, slog, sess.items, sess.n_items);
    /* Surface the session id on stderr (stdout is the model's answer, kept
     * clean for piping) so a one-shot run can be picked up with --resume.
     * NULL when nothing was recorded or persistence is disabled. */
    const char *hint = session_log_resume_hint(slog);
    /* Run stats mirror the REPL's per-turn line (context · time · spend),
     * printed only when a backend actually reported something — a provider
     * that sends no usage keeps -p output free of a "context ?" stub. Time
     * alone isn't worth a line. */
    /* Total spend: reported cost, plus each unreported response priced
     * against the catalog (spend_total). Approximate ("~$") whenever
     * unreported usage exists at all — priced or not, a reported subtotal
     * must never display as an exact grand total. */
    if (spend_unpriced(&costs)) {
        /* Unpriced usage the cache couldn't answer for — likely a cold
         * cache racing the download this run started. Give the fetch a
         * bounded moment to land instead of letting shutdown cancel it,
         * then reprice: this run's estimate can resolve, and repeated
         * short -p runs can't keep a cold cache cold forever. Deliberately
         * conditional — when a stale-but-usable snapshot already priced
         * the run, exit latency wins over refresh eagerness (a hanging
         * endpoint would otherwise tax every run 3s while the mtime stays
         * old), and the 30-day staleness alarm still backstops real rot. */
        catalog_drain(3000);
    }
    int spend_approx = 0;
    double spend = spend_total(&costs, &spend_approx);
    int have_stats = last_ctx >= 0 || spend > 0;
    if (hint || have_stats) {
        /* Flush the answer (block-buffered when stdout is piped) before the
         * unbuffered stderr write, so a combined 2>&1 stream shows the hint
         * after the answer rather than racing ahead of it. */
        fflush(stdout);
        /* Always lead with a blank line so the footnotes read as such
         * rather than part of the answer — agents commonly fold stderr
         * into stdout (2>&1), where the model's output would otherwise run
         * straight into them. Dim only on a terminal; a captured log stays
         * plain (no stray ANSI). */
        int tty = isatty(fileno(stderr));
        fputc('\n', stderr);
        if (have_stats) {
            /* Same field selection/formatting as the REPL stats line
             * (display_stats_line), minus the reflow — stderr footnotes
             * are plain single lines. */
            char segs[STATS_SEGS_MAX][STATS_SEG_LEN];
            int n = format_stats_segments(segs, last_ctx, compact_context_limit(p, sess.model),
                                          monotonic_ms() - start_ms, spend, spend_approx);
            fputs(tty ? ANSI_DIM : "", stderr);
            for (int i = 0; i < n; i++)
                fprintf(stderr, "%s%s", i ? " · " : "", segs[i]);
            fprintf(stderr, "%s\n", tty ? ANSI_RESET : "");
        }
        if (hint)
            fprintf(stderr, "%sresume with: hax --resume=%s%s\n", tty ? ANSI_DIM : "", hint,
                    tty ? ANSI_RESET : "");
    }
    transcript_log_close(tlog);
    session_log_close(slog);
    spend_free(&costs);
    agent_session_free(&sess);
    return rc;
}
