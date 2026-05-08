/* SPDX-License-Identifier: MIT */
#include "oneshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "ctrl_strip.h"
#include "tool.h"
#include "transcript.h"
#include "turn.h"
#include "util.h"

/* Per-stream callback state for the oneshot path. We hand turn.c every
 * event for item assembly and capture the human-readable error text on
 * EV_ERROR (turn sets t->error but doesn't carry the message). */
struct oneshot_ctx {
    struct turn *turn;
    char *error_msg; /* malloc'd; set on EV_ERROR */
};

static int on_event(const struct stream_event *ev, void *user)
{
    struct oneshot_ctx *oc = user;
    if (ev->kind == EV_ERROR && !oc->error_msg && ev->u.error.message)
        oc->error_msg = xstrdup(ev->u.error.message);
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

int oneshot_run(struct provider *p, const char *prompt, const struct hax_opts *opts, int max_turns)
{
    struct agent_session sess;
    if (agent_session_init(&sess, p, opts) < 0)
        return 1;

    /* HAX_TRANSCRIPT mirror, opened (and the file truncated) before
     * any model call so even an early-error or zero-turn run leaves
     * a fresh file behind. NULL when the env var is unset; all
     * transcript_log_* entry points are NULL-safe. Appended at the end
     * of each round-trip so `tail -f` works during long agentic runs;
     * the final append at `done:` catches the no-tool exit path and
     * is idempotent on the others (transcript_log_append no-ops when
     * n_items hasn't grown). */
    struct transcript_log *tlog = transcript_log_open(sess.sys, sess.tools, sess.n_tools);

    agent_session_add_user(&sess, prompt);
    /* Land the prompt on disk before any provider call so a hang or
     * crash mid-stream still leaves the triggering input visible in
     * the log. */
    transcript_log_append(tlog, sess.items, sess.n_items);

    int rc = 0;
    int first_inner = 1;
    for (int turn_n = 0; turn_n < max_turns; turn_n++) {
        if (!first_inner) {
            agent_session_add_boundary(&sess);
            /* Same: flush the new turn rule before the next stream. */
            transcript_log_append(tlog, sess.items, sess.n_items);
        }
        first_inner = 0;

        struct context ctx = agent_session_context(&sess);

        struct turn t;
        turn_init(&t);
        struct oneshot_ctx oc = {.turn = &t, .error_msg = NULL};
        /* Oneshot doesn't arm interrupt and has no idle UI to surface
         * — pass a NULL tick so http skips the progress hook entirely. */
        p->stream(p, &ctx, sess.model, on_event, &oc, NULL, NULL);

        if (t.error) {
            fprintf(stderr, "hax: provider error: %s\n",
                    oc.error_msg ? oc.error_msg : "(no message)");
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
            if (tool)
                result = tool->run(sess.items[i].tool_arguments_json, NULL, NULL);
            else if (sess.n_tools == 0)
                result = xstrdup("error: tool calls are disabled in this session");
            else
                result = xasprintf("unknown tool: %s", sess.items[i].tool_name);
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
        transcript_log_append(tlog, sess.items, sess.n_items);
    }

    /* Loop fell through without a final assistant-only response. */
    fprintf(stderr, "hax: max turns (%d) exceeded; aborting\n", max_turns);
    rc = 1;

done:
    transcript_log_append(tlog, sess.items, sess.n_items);
    transcript_log_close(tlog);
    agent_session_free(&sess);
    return rc;
}
