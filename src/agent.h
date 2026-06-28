/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_H
#define HAX_AGENT_H

#include "agent_core.h"
#include "provider.h"

struct transcript_log;
struct session_log;
struct render_ctx;

/* Live REPL state owned by agent_run and borrowed by slash handlers. */
struct agent_state {
    struct agent_session *sess;
    const struct provider *provider;
    struct transcript_log *tlog;
    /* Append-only session record for resume; NULL when persistence is
     * disabled (HAX_NO_SESSION) or unavailable. */
    struct session_log *slog;
    /* Live render state borrowed from agent_run, used by handlers that replay
     * or redraw conversation state. */
    struct render_ctx *r;
};

/* Run the interactive REPL. `*provider` is owned by the caller but may be
 * replaced by /provider; on return it points at the live provider to destroy. */
int agent_run(struct provider **provider, const struct hax_opts *opts);

/* Print the two-row startup banner; caller owns surrounding spacing. */
void agent_print_banner(const struct provider *p, const struct agent_session *s);

/* Implement /new: clear history, rewrite HAX_TRANSCRIPT, and reprint banner. */
void agent_new_conversation(struct agent_state *st);

/* Implement /resume: load `path`, continue that session file, mirror it to
 * HAX_TRANSCRIPT, and replay only the last user turn for orientation. On load
 * failure, report and leave the current conversation untouched. */
void agent_resume_session(struct agent_state *st, const char *path);

/* Install freshly constructed `newp`, destroying the previous provider. The
 * caller follows with agent_apply_settings to resolve model/effort and prompt. */
void agent_set_provider(struct agent_state *st, struct provider *newp);

/* Apply /provider, /model, or /effort: resolve settings, rebuild the prompt,
 * refresh logs/display, and leave history intact on missing model. */
int agent_apply_settings(struct agent_state *st);

/* Summarize and replace history with a seed message, rotating logs. Optional
 * `instructions` focus the summary; `is_auto` suppresses no-op chatter for the
 * threshold-triggered path. Returns 1 when compacted, else 0 with history intact. */
int agent_compact(struct agent_state *st, const char *instructions, int is_auto);

#endif /* HAX_AGENT_H */
