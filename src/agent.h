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
    long input_tokens;       /* summed across round-trips (each sends the full context) */
    long output_tokens;      /* total tokens generated */
    long cached_tokens;      /* total input tokens served from the prefix cache */
    long cache_write_tokens; /* total input tokens billed as cache writes */
    /* Spend accounting (agent_core.h): exact provider-reported cost plus
     * per-request records of responses that reported none, each stamped
     * with the catalog identity it ran under. Records are priced at
     * *render* time — so a late-landing catalog fetch retroactively
     * covers earlier turns — and each at its own request's rates, so
     * provider/model switches need no settling and context-tier pricing
     * sees true per-request input sizes. Owns heap memory: release with
     * spend_free before zeroing the stats. */
    struct spend_totals spend;
    long worked_ms; /* wall time spent inside user turns */
    long turns;     /* user turns started by typed input; empty-send
                     * continuations of a resumable turn don't re-count */
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

/* Why the last user turn stopped short of completion — the REPL's
 * resumable state. While set, an empty send at the prompt continues the
 * turn (a dim hint line above the prompt says so), and a typed message
 * steers it; agent_run rederives it from every loop outcome, and any
 * operation that rewrites history wholesale (/new, /resume, /undo,
 * /fork, manual /compact) clears it back to NONE. */
enum agent_resume {
    AGENT_RESUME_NONE = 0,
    AGENT_RESUME_PAUSED,      /* soft interrupt stopped the loop at a turn seam */
    AGENT_RESUME_MAX_TURNS,   /* the per-user-turn round-trip budget ran out */
    AGENT_RESUME_INTERRUPTED, /* hard interrupt; abort repair left markers */
    AGENT_RESUME_ERROR,       /* provider error; empty send retries */
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
    /* A prompt the last /undo or /fork discarded, for the REPL to push onto
     * recall *after* the command line so the first Up-arrow reaches the prompt,
     * not the /undo/fork line. Owned; consumed and freed per slash command. */
    char *pending_recall;
    /* Resumable-turn state (see enum agent_resume). resume_marked says the
     * stopped turn carries interrupt markers, so an empty-send resume must
     * append the CONTINUE_MARKER user message rather than nothing. */
    enum agent_resume resume;
    int resume_marked;
    /* The end-of-turn auto-compaction was owed but skipped because the run
     * ended with an Esc or short of completion; the next send settles it
     * before its request. An explicit flag, not re-derived from last_ctx,
     * so a compaction that runs and fails degrades to one attempt — not a
     * retry ahead of every future send. */
    int compact_deferred;
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

/* Number of user turns in the live conversation — non-seed user messages,
 * the unit /undo and /fork act on. */
size_t agent_user_turn_count(const struct agent_session *s);

/* The prompt text of user turn `turn` (0-based, oldest first), borrowed and
 * valid until the next mutation of history. NULL when `turn` is out of range.
 * Used by the /undo and /fork pickers to label rows. */
const char *agent_user_turn_text(const struct agent_session *s, size_t turn);

/* Revert the live conversation to just before user turn `turn` (0-based):
 * drop turns [turn, end) from history, the session file, and the transcript
 * mirror; seed the discarded prompt into editor recall; and replay the new
 * tail behind a dim "── undid … ──" rule. `turn` must be <
 * agent_user_turn_count (the caller validates). Used by /undo. */
void agent_undo(struct agent_state *st, size_t turn);

/* Fork a new branch before user turn `turn` (0-based): copy the session's
 * first `turn` turns into a fresh file (the original is left whole, resumable
 * as the pre-fork branch), switch the live session and logs onto the copy,
 * truncate history to the branch point, seed the discarded prompt into recall,
 * and replay the new tail behind a dim "── forked ──" rule. `turn` >=
 * agent_user_turn_count forks at the tip (a whole-conversation clone, nothing
 * discarded). Requires session recording: with it off or unavailable there is
 * no original to preserve, so it reports an error and leaves history intact.
 * Used by /fork. */
void agent_fork(struct agent_state *st, size_t turn);

/* Re-resolve model + reasoning effort against `p`, rebuild the system prompt,
 * and confirm the change on screen: a dim one-line "switched to …" marker
 * mid-conversation, or a fresh banner while the conversation is empty. When
 * p differs from the live provider, this is an atomic provider replacement:
 * validate/reconfigure first, then transfer ownership into st and destroy the
 * old provider. A provider or model change refreshes model-specific context;
 * a same-provider effort change does not. Returns 0 on success. On failure,
 * the session and live provider stay intact; when p is a prospective
 * replacement, the caller retains ownership of it. */
int agent_apply_settings(struct agent_state *st, struct provider *p);

/* Total session spend for display, USD: provider-reported cost plus the
 * catalog-estimated cost of unreported responses, each priced at its own
 * recorded catalog identity (spend_total). Sets *approx (when non-NULL)
 * to 1 iff any inexact component exists, so callers can mark the figure
 * ("~$0.42"). */
double agent_session_spend(const struct session_stats *t, int *approx);

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
