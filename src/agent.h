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

/* Run the interactive REPL. *provider is the initial provider (owned by the
 * caller); a runtime /provider switch may replace it, destroying the old
 * one and storing the new current provider back into *provider. On return the
 * caller destroys *provider — which is whatever provider is live at exit, not
 * necessarily the one passed in. */
int agent_run(struct provider **provider, const struct hax_opts *opts);

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

/* Replace the live provider with `newp` (freshly constructed; ownership
 * transfers in). Destroys the previously-live provider — joining its
 * background work — and points the session at `newp`. Does NOT re-resolve
 * model/effort or rebuild the prompt; the caller follows with
 * agent_apply_settings once the new model/effort are chosen. Used by the
 * /provider switch. */
void agent_set_provider(struct agent_state *st, struct provider *newp);

/* Re-resolve model + reasoning effort against the current config and the
 * live provider, rebuild the system prompt, append a dim marker to history
 * (so the switch shows in the transcript and survives a resume), and print
 * a one-line confirmation. The single "apply a /provider, /model or /effort
 * change" step. Returns 0 on success, -1 when no model resolves for the
 * provider (a note is printed; history is left intact). */
int agent_apply_settings(struct agent_state *st);

/* Summarize the live conversation and replace history with the summary,
 * so the session can continue without overflowing the context window.
 * Streams a tool-free summarization request (showing a "compacting..."
 * spinner), then swaps history for a single seed user message and rotates
 * the session/transcript logs. `instructions` is optional extra focus for
 * the summary (NULL/empty for none). `is_auto` distinguishes the threshold-
 * triggered path (quiet on a no-op) from manual /compact. Returns 1 when
 * history was compacted, 0 on failure/cancel/empty/no-op (history intact).
 * Used by the /compact command and the post-user-turn auto-trigger. */
int agent_compact(struct agent_state *st, const char *instructions, int is_auto);

#endif /* HAX_AGENT_H */
