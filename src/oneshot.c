/* SPDX-License-Identifier: MIT */
#include "oneshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "compact.h"
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

/* Oneshot stream state: assemble the turn and retain EV_ERROR's message. */
struct oneshot_ctx {
    struct turn *turn;
    char *error_msg; /* malloc'd; set on EV_ERROR */
    /* From EV_DONE; -1 fields mean unreported. Used for auto-compaction. */
    struct stream_usage usage;
};

static int on_event(const struct stream_event *ev, void *user)
{
    struct oneshot_ctx *oc = user;
    if (ev->kind == EV_ERROR && !oc->error_msg && ev->u.error.message)
        oc->error_msg = xstrdup(ev->u.error.message);
    else if (ev->kind == EV_DONE)
        oc->usage = ev->u.done.usage;
    turn_on_event(ev, oc->turn);
    return 0;
}

/* Print every assistant message produced after `from`; multiple messages are
 * legal, so don't silently drop extras. */
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

/* Non-interactive compaction: summarize, replace history, and rotate logs.
 * Returns 1 if compacted, 0 on empty/failure with history intact. */
static int oneshot_compact(struct agent_session *s, struct provider *p, struct session_log *slog,
                           struct transcript_log *tlog)
{
    if (s->n_items == 0)
        return 0;

    struct turn t;
    turn_init(&t);
    /* Reuse the normal sink (feeds the turn, captures any error); no tick,
     * no display. */
    struct oneshot_ctx oc = {.turn = &t, .error_msg = NULL, .usage = {-1, -1, -1}};
    char *summary = compact_summarize(s, p, NULL, &t, on_event, &oc, NULL, NULL);
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
    /* No interactive picker in -p mode, so a missing model is fatal. */
    if (!sess.model || !*sess.model) {
        hax_err("HAX_MODEL is required for provider '%s' (no default)", p->name ? p->name : "?");
        agent_session_free(&sess);
        return 1;
    }

    /* Seed resumed history before the new prompt; unreadable sessions are fatal
     * rather than silently running against empty context. */
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

    /* Open/truncate HAX_TRANSCRIPT before streaming. Per-round flushes support
     * `tail -f`; the final flush is idempotent and catches the no-tool path. */
    struct transcript_log *tlog = transcript_log_open(sess.sys, sess.tools, sess.n_tools);
    /* Append-only session record — continue the resumed file, else begin
     * a fresh one. NULL when persistence is disabled. */
    struct session_log *slog = opts->resume_path
                                   ? session_log_resume(opts->resume_path, p->name, sess.model,
                                                        sess.reasoning_effort, n_resumed)
                                   : session_log_open(p->name, sess.model, sess.reasoning_effort);
    if (n_resumed > 0)
        transcript_log_append(tlog, sess.items, sess.n_items);

    agent_session_add_user(&sess, prompt);
    /* Log the prompt before streaming so hangs/crashes keep the trigger visible. */
    oneshot_flush(tlog, slog, sess.items, sess.n_items);

    /* Prevent idle sleep across unattended -p runs; released at `done:` and
     * no-op when keep_awake is off or unsupported. */
    keepawake_acquire();

    int rc = 0;
    int first_inner = 1;
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
        struct oneshot_ctx oc = {.turn = &t, .error_msg = NULL, .usage = {-1, -1, -1}};
        /* No interrupt or idle UI in -p; skip the progress hook. */
        p->stream(p, &ctx, sess.model, on_event, &oc, NULL, NULL);

        if (t.error) {
            hax_err("provider error: %s", oc.error_msg ? oc.error_msg : "(no message)");
            free(oc.error_msg);
            turn_reset(&t);
            rc = 1;
            goto done;
        }
        free(oc.error_msg);

        /* Latest reported window size for the between-round-trip compaction check. */
        long round_ctx = (oc.usage.input_tokens >= 0 && oc.usage.output_tokens >= 0)
                             ? oc.usage.input_tokens + oc.usage.output_tokens
                             : -1;

        size_t n_before;
        int had_tool_call;
        agent_session_absorb(&sess, &t, &n_before, &had_tool_call);
        turn_reset(&t);

        if (!had_tool_call) {
            print_assistant_text(sess.items, n_before, sess.n_items);
            goto done;
        }

        /* Run tools silently but normalize history like the REPL. If --raw
         * advertised no tools, refuse any returned tool_call instead of running it. */
        size_t current_end = sess.n_items;
        for (size_t i = n_before; i < current_end; i++) {
            if (sess.items[i].kind != ITEM_TOOL_CALL)
                continue;
            const struct tool *tool = sess.n_tools ? find_tool(sess.items[i].tool_name) : NULL;
            char *result;
            if (tool) {
                /* Match interactive per-tool arg normalization. */
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
        /* Flush only after CALL/RESULT pairs are complete for this round-trip. */
        oneshot_flush(tlog, slog, sess.items, sess.n_items);

        /* If this tool round-trip nears the window, compact before the next
         * request and note it quietly on stderr for `tail -f` logs. */
        if (compact_should_auto(round_ctx, compact_context_limit(p)) &&
            oneshot_compact(&sess, p, slog, tlog)) {
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
    /* Put the resume hint on stderr so stdout stays pipeable. */
    const char *hint = session_log_resume_hint(slog);
    if (hint) {
        /* Flush stdout first so combined 2>&1 logs show the hint after the answer. */
        fflush(stdout);
        /* Blank line makes the hint a footnote under 2>&1; dim only on a TTY. */
        int tty = isatty(fileno(stderr));
        fprintf(stderr, "\n%sresume with: hax --resume=%s%s\n", tty ? ANSI_DIM : "", hint,
                tty ? ANSI_RESET : "");
    }
    transcript_log_close(tlog);
    session_log_close(slog);
    agent_session_free(&sess);
    return rc;
}
