/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_H
#define HAX_AGENT_H

#include "agent_core.h"
#include "provider.h"

struct transcript_log;
struct session_log;
struct render_ctx;

/* Cumulative usage totals for the running REPL, accumulated across user
 * turns and reset by /new alongside the conversation. Only provider-
 * reported numbers are summed (-1 "not reported" fields are skipped), so
 * a zero can also mean "the backend never said". Deliberately process-
 * lifetime: /resume restores the conversation, not the resumed session's
 * historical totals — "time worked" and "spend" describe this sitting. */
struct session_stats {
    long input_tokens;  /* summed across round-trips (each sends the full context) */
    long output_tokens; /* total tokens generated */
    long cached_tokens; /* total input tokens served from the prefix cache */
    /* Spend accounting (agent_core.h): exact provider-reported cost plus
     * the open pricing segment — responses that reported none, accumulated
     * since the last provider/model switch. The segment is priced at
     * *render* time against the current model's catalog rates — so a
     * late-landing catalog fetch retroactively covers earlier turns — and
     * folded into est_cost at the then-current rates by the settle step
     * when a switch changes the rates mid-session. */
    struct spend_totals spend;
    /* Catalog-estimated spend of *settled* pricing segments, USD.
     * Displayed added to spend and marked approximate ("~$"). */
    double est_cost;
    /* Set when a settle had to *drop* segment tokens it couldn't price
     * (catalog fetch never landed, model unknown to it): the spend total
     * is missing real usage from then on, so it must stay marked
     * approximate even when every remaining component is exact. */
    int est_dropped;
    long worked_ms; /* wall time spent inside user turns */
    long turns;     /* user turns run */
    long requests;  /* model round-trips streamed (glossary: turns) */
    /* Tool invocations the model made, total and per type. Per-type slots
     * key on the registry's static tool names (find_tool), which outlive
     * items — an item-owned name would dangle once compaction frees the
     * history that carried it. Slots fill in first-use order; calls to
     * unregistered names count only toward the total. */
    long tool_calls;
#define SESSION_STATS_MAX_TOOLS 8
    struct {
        const char *name; /* borrowed static registry name; NULL = free slot */
        long count;
    } tools[SESSION_STATS_MAX_TOOLS];
    /* Current window state: input+output of the latest reported response —
     * the same number the per-turn stats line shows, NOT a sum. Kept so
     * /session can present the window frame next to the billing-frame
     * totals above (summed `in` exceeds it as soon as there's a second
     * round-trip, which reads as a bug without this anchor). 0 = none
     * reported yet. last_limit is the context window resolved alongside it
     * (0 = unknown), snapshotted here so /session doesn't have to reach
     * into the provider/compact layers. */
    long last_ctx;
    long last_limit;
};

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
    /* Session usage totals — read by /session, reset by /new. */
    struct session_stats stats;
};

/* Run the interactive REPL. *provider is the initial provider (owned by the
 * caller); a runtime /provider switch may replace it, destroying the old
 * one and storing the new current provider back into *provider. On return the
 * caller destroys *provider — which is whatever provider is live at exit, not
 * necessarily the one passed in. */
int agent_run(struct provider **provider, const struct hax_opts *opts);

/* Print the two-line startup banner: provider/model identification
 * and the key-tip line. Reused by agent_new_conversation (a fresh-
 * conversation reset shows the same banner the user saw at startup)
 * and by agent_apply_settings on an empty conversation (a switch
 * before the first prompt replaces the stale startup banner rather
 * than whispering under it). The caller emits the leading blank-line
 * gap; the banner itself is just the two rows. */
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
 * live provider, rebuild the system prompt, and confirm the change on
 * screen: a dim one-line "switched to …" marker mid-conversation, or a
 * fresh banner when the conversation is still empty (so the stale startup
 * banner doesn't outshout the correction). The single "apply a /provider,
 * /model or /effort change" step. Returns 0 on success, -1 when no model
 * resolves for the provider (a note is printed; history is left intact). */
int agent_apply_settings(struct agent_state *st);

/* Total session spend for display, USD: provider-reported cost plus the
 * catalog-estimated cost of unreported responses (settled segments +
 * the open segment priced at the current provider/model's rates). Sets
 * *approx (when non-NULL) to 1 iff any inexact component exists — an
 * estimate contributed, unpriced usage sits in the open segment, or a
 * settle dropped unpriceable tokens — so callers can mark the figure
 * ("~$0.42"). `p`/`model` may be NULL — the open segment then simply
 * can't be priced (and keeps the figure approximate). */
double agent_session_spend(const struct session_stats *t, const struct provider *p,
                           const char *model, int *approx);

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
