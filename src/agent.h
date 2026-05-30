/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_H
#define HAX_AGENT_H

#include "agent_core.h"
#include "provider.h"

struct transcript_log;
struct session_log;
struct render_ctx;

/* Live REPL state owned by agent_run, exposed to slash handlers (and
 * any future callers that need to mutate the conversation) so a single
 * pointer is enough to act on history, the optional HAX_TRANSCRIPT log,
 * and the provider in lockstep. Lifetime: stack-allocated in agent_run;
 * never heap-owned, never outlives the REPL frame. */
struct agent_state {
    struct agent_session *sess;
    const struct provider *provider;
    struct transcript_log *tlog;
    /* Append-only session record for resume; NULL when persistence is
     * disabled (HAX_NO_SESSION) or unavailable. */
    struct session_log *slog;
    /* The live render state, so mid-session handlers (e.g. /resume) can
     * drive the same display pipeline agent_run uses — replaying the
     * restored conversation through it. Points at agent_run's stack
     * frame; never heap-owned. */
    struct render_ctx *r;
};

int agent_run(struct provider *p, const struct hax_opts *opts);

/* Print the two-line startup banner: provider/model identification
 * and the key-tip line. Reused by agent_new_conversation so a fresh-
 * conversation reset shows the same banner the user saw at startup.
 * Emits a leading blank line so the banner stands clear of whatever
 * was on the terminal before. */
void agent_print_banner(const struct provider *p, const struct agent_session *s);

/* Reset the live conversation to a fresh state: clear the items vector,
 * truncate-and-rewrite the HAX_TRANSCRIPT log (no-op when unset), and
 * reprint the banner. The single entry point for "what /new means" so
 * future additions (e.g. clearing a future cache, resetting usage
 * counters) don't leak into slash.c. */
void agent_new_conversation(struct agent_state *st);

/* Replace the live conversation with the one stored at `path`: load its
 * items into history, continue appending to that same session file, mirror
 * it into the HAX_TRANSCRIPT log, and replay the last user turn (a dim
 * "resumed" rule, then the final exchange rendered live) for orientation.
 * Earlier history is reachable via Ctrl-T rather than replayed inline.
 * Used by the /resume command. A load failure is reported and leaves the
 * current conversation untouched. */
void agent_resume_session(struct agent_state *st, const char *path);

#endif /* HAX_AGENT_H */
