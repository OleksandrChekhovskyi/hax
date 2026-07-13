/* SPDX-License-Identifier: MIT */
#include "oneshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "catalog.h"
#include "compact.h"
#include "config.h"
#include "session.h"
#include "tool.h"
#include "transcript.h"
#include "turn.h"
#include "util.h"
#include "render/ctrl_strip.h"
#include "system/keepawake.h"
#include "terminal/ansi.h"

/* Advance both append-only logs over the same item slice — see the twin
 * helper in agent.c. Both no-op on a NULL log or no new items. */
static void oneshot_flush(struct transcript_log *tlog, struct session_log *slog,
                          const struct item *items, size_t n)
{
    transcript_log_append(tlog, items, n);
    session_log_append(slog, items, n);
}

/* Per-stream callback state for the oneshot path. We hand turn.c every
 * event for item assembly and capture the human-readable error text on
 * EV_ERROR (turn sets t->error but doesn't carry the message). */
struct oneshot_ctx {
    struct turn *turn;
    char *error_msg; /* malloc'd; set on EV_ERROR */
    /* Filled from EV_DONE; all fields -1 if the provider didn't report.
     * Drives the auto-compaction threshold check between round-trips and
     * the exit stats summary. */
    struct stream_usage usage;
    /* Spend of every EV_DONE this sink saw — unlike `usage`
     * (last-one-wins), accumulated, so a multi-attempt compaction stream
     * (reject-and-retry on a stray tool call) bills every attempt into
     * the exit summary. Folded into the run-level totals per stream; the
     * unreported-token segment is priced against the catalog at exit
     * (the model never changes mid-run here, so no settling). */
    struct spend_totals spend;
};

static int on_event(const struct stream_event *ev, void *user)
{
    struct oneshot_ctx *oc = user;
    if (ev->kind == EV_ERROR) {
        if (!oc->error_msg && ev->u.error.message)
            oc->error_msg = xstrdup(ev->u.error.message);
        /* A truncated response reports its usage on the error — billed
         * like a complete one. */
        if (ev->u.error.usage) {
            oc->usage = *ev->u.error.usage;
            spend_account(&oc->spend, ev->u.error.usage);
        }
    } else if (ev->kind == EV_DONE) {
        oc->usage = ev->u.done.usage;
        spend_account(&oc->spend, &ev->u.done.usage);
    }
    turn_on_event(ev, oc->turn);
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

/* Summarize history into a single seed message in place — the non-interactive
 * twin of agent.c's agent_compact. No display, no tick, no Esc handling: a
 * summarization request, then history is replaced and both logs
 * rotate to fresh files (seeded /new). Returns 1 if compacted, 0 on
 * empty/failure (history left intact); either way *costs accumulates the
 * spend of the summarization round-trips (reported and unreported alike),
 * so the exit stats bill compaction like any other request. Used between
 * round-trips so a long agentic `-p` run survives its own context growth. */
static int oneshot_compact(struct agent_session *s, struct provider *p, struct session_log *slog,
                           struct transcript_log *tlog, struct spend_totals *costs)
{
    if (s->n_items == 0)
        return 0;

    struct turn t;
    turn_init(&t);
    /* Reuse the normal sink (feeds the turn, captures any error); no tick,
     * no display. */
    struct oneshot_ctx oc = {.turn = &t, .error_msg = NULL, .usage = {-1, -1, -1, -1, -1, -1}};
    char *summary = compact_summarize(s, p, NULL, &t, on_event, &oc, NULL, NULL, NULL);
    spend_fold(costs, &oc.spend);
    free(oc.error_msg);
    turn_reset(&t);

    if (!summary || !summary[0]) {
        free(summary);
        return 0;
    }

    compact_apply(s, slog, tlog, summary);
    free(summary);
    return 1;
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
     * the final append at `done:` catches the no-tool exit path and
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
    oneshot_flush(tlog, slog, sess.items, sess.n_items);

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

    /* Keep the machine from idling to sleep across the whole agentic
     * run — a long unattended -p invocation (or one driven from
     * automation) shouldn't be cut short by the idle timer. Released at
     * `done:`, the single cleanup funnel for every exit path below.
     * Gated by the keep_awake config key; no-op when off or where no
     * inhibitor helper exists. */
    keepawake_acquire();

    int rc = 0;
    int first_inner = 1;
    /* Run stats for the exit summary on stderr: latest reported context
     * size, summed provider-reported cost (compaction round-trips
     * included, via oneshot_compact's cost accumulator), wall time for
     * the whole run. */
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
    for (int turn_n = 0; turn_n < max_turns; turn_n++) {
        if (!first_inner) {
            agent_session_add_boundary(&sess);
            /* Same: flush the new turn rule before the next stream. */
            oneshot_flush(tlog, slog, sess.items, sess.n_items);
        }
        first_inner = 0;

        struct context ctx = agent_session_context(&sess);

        struct turn t;
        turn_init(&t);
        struct oneshot_ctx oc = {.turn = &t, .error_msg = NULL, .usage = {-1, -1, -1, -1, -1, -1}};
        /* Oneshot doesn't arm interrupt and has no idle UI to surface
         * — pass a NULL tick so http skips the progress hook entirely. */
        p->stream(p, &ctx, sess.model, on_event, &oc, NULL, NULL);

        /* Accounting runs before the error exit: a truncated response
         * reported its usage on the error and is billed like a complete
         * one. round_ctx is the exact context size this round-trip
         * reported (input subsumes the prior prefix, output is what was
         * just generated) — the signal the between-round-trip
         * auto-compaction check reads below. */
        spend_fold(&costs, &oc.spend);
        long round_ctx = (oc.usage.input_tokens >= 0 && oc.usage.output_tokens >= 0)
                             ? oc.usage.input_tokens + oc.usage.output_tokens
                             : -1;
        if (round_ctx >= 0)
            last_ctx = round_ctx;

        if (t.error) {
            hax_err("provider error: %s", oc.error_msg ? oc.error_msg : "(no message)");
            free(oc.error_msg);
            turn_reset(&t);
            rc = 1;
            goto done;
        }
        free(oc.error_msg);

        size_t n_before;
        int had_tool_call;
        agent_session_absorb(&sess, &t, &n_before, &had_tool_call);
        turn_reset(&t);

        if (!had_tool_call) {
            print_assistant_text(sess.items, n_before, sess.n_items);
            goto done;
        }

        /* Run tools silently — no display, no spinner. The result is
         * ctrl_strip'd at the boundary just like the verbose path so
         * the conversation lands in history in the same normalized
         * form. emit_display callback is NULL: streamed output would
         * have nowhere to go.
         *
         * --raw gate (sess.n_tools == 0): advertised no tools to the
         * provider, so a tool_call coming back is either a model bug
         * or a malicious backend. Refuse to execute and feed an error
         * result to the model — the same gate exists in agent.c. */
        size_t current_end = sess.n_items;
        for (size_t i = n_before; i < current_end; i++) {
            if (sess.items[i].kind != ITEM_TOOL_CALL)
                continue;
            const struct tool *tool = sess.n_tools ? find_tool(sess.items[i].tool_name) : NULL;
            char *result;
            if (tool) {
                /* Apply the same per-tool arg normalization as
                 * interactive dispatch (agent.c::dispatch_tool_call) so
                 * tool behavior doesn't silently differ between modes. */
                char *rewritten = NULL;
                if (tool->preprocess_args && sess.items[i].tool_arguments_json)
                    rewritten = tool->preprocess_args(sess.items[i].tool_arguments_json);
                const char *args = rewritten ? rewritten : sess.items[i].tool_arguments_json;
                result = tool->run(args, NULL, NULL);
                free(rewritten);
            } else if (sess.n_tools == 0) {
                result = xstrdup("error: tool calls are disabled in this session");
            } else {
                result = xasprintf("unknown tool: %s", sess.items[i].tool_name);
            }
            char *history = ctrl_strip_dup(result);
            free(result);
            items_append(&sess.items, &sess.n_items, &sess.cap_items,
                         (struct item){
                             .kind = ITEM_TOOL_RESULT,
                             .call_id = xstrdup(sess.items[i].call_id),
                             .output = history,
                         });
        }
        /* Round-trip complete (assistant + tool calls + their results
         * all in sess.items): flush. CALL/RESULT pairing is intact for
         * the renderer's lookahead. */
        oneshot_flush(tlog, slog, sess.items, sess.n_items);

        /* Between-round-trip auto-compaction: the model called tools and we
         * loop for another request. If this round-trip's reported usage nears
         * the window, summarize the completed work now so a long agentic chain
         * doesn't run itself into an overflow. The next iteration streams from
         * the seed. Quiet on stderr so a `tail`'d -p log shows it happened. */
        if (compact_should_auto(round_ctx, compact_context_limit(p, sess.model)) &&
            oneshot_compact(&sess, p, slog, tlog, &costs)) {
            int tty = isatty(fileno(stderr));
            fprintf(stderr, "%s[compacted context]%s\n", tty ? ANSI_DIM : "",
                    tty ? ANSI_RESET : "");
        }
    }

    /* Loop fell through without a final assistant-only response. */
    hax_err("max turns (%d) exceeded; aborting", max_turns);
    rc = 1;

done:
    keepawake_release();
    oneshot_flush(tlog, slog, sess.items, sess.n_items);
    /* Surface the session id on stderr (stdout is the model's answer, kept
     * clean for piping) so a one-shot run can be picked up with --resume.
     * NULL when nothing was recorded or persistence is disabled. */
    const char *hint = session_log_resume_hint(slog);
    /* Run stats mirror the REPL's per-turn line (context · time · spend),
     * printed only when a backend actually reported something — a provider
     * that sends no usage keeps -p output free of a "context ?" stub. Time
     * alone isn't worth a line. */
    /* Total spend: reported cost, plus the unreported responses' tokens
     * priced against the catalog. Approximate ("~$") whenever unreported
     * usage exists at all — priced or not, a reported subtotal must never
     * display as an exact grand total. */
    double spend = costs.reported;
    double est = spend_estimate(&costs, p, sess.model);
    if (est <= 0 && spend_has_tokens(&costs)) {
        /* Unpriced usage the cache couldn't answer for — likely a cold
         * cache racing the download this run started. Give the fetch a
         * bounded moment to land instead of letting shutdown cancel it,
         * then retry once: this run's estimate can resolve, and repeated
         * short -p runs can't keep a cold cache cold forever. Deliberately
         * conditional — when a stale-but-usable snapshot already priced
         * the run, exit latency wins over refresh eagerness (a hanging
         * endpoint would otherwise tax every run 3s while the mtime stays
         * old), and the 30-day staleness alarm still backstops real rot. */
        catalog_drain(3000);
        est = spend_estimate(&costs, p, sess.model);
    }
    if (est > 0)
        spend += est;
    int spend_approx = est > 0 || spend_has_tokens(&costs);
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
             * (display_stats_line), minus the verbose detail and reflow —
             * stderr footnotes are plain single lines. */
            char segs[STATS_SEGS_MAX][STATS_SEG_LEN];
            int n = format_stats_segments(segs, last_ctx, compact_context_limit(p, sess.model), -1,
                                          -1, 0, monotonic_ms() - start_ms, spend, spend_approx);
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
    agent_session_free(&sess);
    return rc;
}
