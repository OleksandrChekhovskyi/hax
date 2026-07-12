/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_CORE_H
#define HAX_AGENT_CORE_H

#include <stddef.h>

#include "provider.h"
#include "tool.h"
#include "turn.h"

/* Sentinel inserted into conversation history (assistant text or a
 * synthesized tool_result) to mark a turn that was cut short — by Esc
 * cancellation, a mid-stream error, or a tool skipped partway through a
 * batch. Keeps history well-formed and signals the model on the next
 * turn that the prior response was incomplete. Shared so both the REPL
 * loop (history shaping) and dispatch (the rendered skip block) agree. */
#define INTERRUPT_MARKER "[interrupted]"

/* Run-wide options parsed from CLI flags. Lives here (not in agent.h)
 * so the one-shot path can consume it without dragging in the
 * interactive REPL header. */
struct hax_opts {
    /* Bypass system prompt, env block, AGENTS.md, skills, and tools.
     * Sends only the user's message — useful for testing models on
     * raw tasks or as a barebones chat interface. */
    int raw;
    /* Resume a prior conversation: path to a session .jsonl whose items
     * are loaded into history before the loop starts, with new turns
     * appended to the same file. NULL = start a fresh session. Resolved
     * by main.c from --continue / --resume. */
    const char *resume_path;
    /* The provider was auto-selected at cold start (nothing configured),
     * default included, rather than explicitly named. The one-shot stderr
     * banner marks the pick — -p has no /provider picker where the "why"
     * would otherwise show. */
    int provider_autoselected;
};

const struct tool *find_tool(const char *name);

/* Append `it` into a malloc'd vector, doubling capacity on overflow. The
 * three pointers form the canonical "items" vector used in agent.c and
 * oneshot.c — extracted here so both paths grow it the same way. */
void items_append(struct item **items, size_t *n, size_t *cap, struct item it);

/* Resolve the `reasoning_effort` value to send for this session.
 * Empty HAX_REASONING_EFFORT means "force omit"; unset falls back to
 * the provider default; any non-empty value passes through verbatim. */
const char *resolve_reasoning_effort(const struct provider *p);

/* Build the assembled system prompt (base text + env block + AGENTS.md +
 * skills) using `model_label` in the env block. Returns malloc'd, or NULL
 * when the system message should be omitted entirely.
 *
 *   raw=1               → NULL (--raw means "just the prompt, nothing else")
 *   HAX_SYSTEM_PROMPT=""→ NULL (explicit opt-out, same as before)
 *   HAX_SYSTEM_PROMPT   → that value + agent_env_build_suffix(model_label)
 *   unset               → built-in default + agent_env_build_suffix(model_label)
 */
char *build_system_prompt(const char *model_label, int raw);

/* Shared assembly for the stats line shown after each REPL user turn and
 * at the end of a -p run. The default form is terse and unlabeled —
 * "42s", "8.9k / 256k (3%)", "$0.042" — one gauge plus fields whose
 * units label themselves. `verbose` switches to the fully labeled
 * diagnostic form and adds the token detail: "worked 42s", "out 595",
 * "context 8.9k / 256k (3%)", "cached 2.7k", "spent $0.042". Fills `segs`
 * in display order (scope order: turn activity, window state, session
 * total) and returns the count (0 when nothing was reported).
 * Unreported fields are skipped rather than rendered as fake zeros:
 * ctx < 0, elapsed_ms < 0, spend <= 0 (cached only when > 0). Callers
 * own the layout — the REPL reflows segments at the " · " seams, oneshot
 * joins them into one stderr line — so what is shown can never drift
 * between the two. spend_approx marks the spend as a catalog estimate
 * rather than a provider-reported charge: "~$0.042". */
#define STATS_SEGS_MAX 5
#define STATS_SEG_LEN  64
int format_stats_segments(char segs[][STATS_SEG_LEN], long ctx, long limit, long out, long cached,
                          int verbose, long elapsed_ms, double spend, int spend_approx);

/* Spend accounting shared by the REPL and the -p path, so the exact-vs-
 * estimated policy lives in exactly one place (like format_stats_segments
 * does for the display side): a response that reports cost is exact and
 * sums into `reported`; one that doesn't contributes its token counts to
 * the open pricing segment, priced against catalog rates when read. */
struct spend_totals {
    double reported; /* provider-reported cost sum, USD */
    /* The open pricing segment: token sums of responses that reported no
     * cost. Non-overlapping subsets cached/cache_write (and its 1h-TTL
     * subset cache_write_1h) follow the stream_usage conventions. */
    long seg_input;
    long seg_output;
    long seg_cached;
    long seg_cache_write;
    long seg_cache_write_1h;
};

/* Account one completed response into `t` — the single definition of the
 * reported-vs-estimated split. */
void spend_account(struct spend_totals *t, const struct stream_usage *u);

/* Fold `src` into `dst`. For callers that accumulate per-stream sinks
 * (oneshot recreates its event context every round-trip). */
void spend_fold(struct spend_totals *dst, const struct spend_totals *src);

/* True when the open pricing segment holds any tokens — i.e. unreported
 * usage exists that spend_estimate may or may not be able to price. The
 * shared guard that keeps "is there anything to price" and "did pricing
 * fail on real usage" from drifting apart at the call sites. */
int spend_has_tokens(const struct spend_totals *t);

/* Price the open pricing segment against the current provider/model's
 * catalog rates, USD. -1 when it can't be priced: no segment tokens, no
 * catalog identity, or the model is unknown to the catalog. */
double spend_estimate(const struct spend_totals *t, const struct provider *p, const char *model);

/* Live per-run state shared by the interactive and one-shot paths.
 * Owns the items vector, the assembled system prompt, the tools table,
 * and the resolved model + reasoning_effort. */
struct agent_session {
    /* Owned copies, not borrowed config_str pointers: a runtime /provider,
     * /model, or /effort commit replaces a whole config tier object (and frees
     * its strings), which would dangle a borrowed pointer the still-live
     * session keeps using. */
    char *model;               /* owned exact id; NULL/"" = no model resolved yet */
    char *model_label;         /* owned display/env label; NULL when model is NULL */
    char *reasoning_effort;    /* owned; NULL = "omit" */
    const char *provider_name; /* borrowed from the live provider (valid for
                                * its lifetime), for stamping reasoning
                                * items. NULL = none. */
    char *sys;                 /* owned; NULL = no system message */
    struct tool_def *tools;    /* owned; NULL when n_tools == 0 */
    size_t n_tools;
    /* The --raw decision, captured at init so a mid-session provider/model
     * switch (agent_session_reconfigure) rebuilds the system prompt the
     * same way: raw stays "no system message", non-raw refreshes the env
     * block with the new model name. */
    int raw;

    struct item *items;
    size_t n_items;
    size_t cap_items;
};

/* Resolve model + build sys + tools table. Always returns 0: a missing
 * model (provider with no default, nothing configured) is left as an empty
 * s->model rather than failing, so the interactive REPL can start and let
 * the user pick one at runtime. Callers that can't prompt (the one-shot
 * path) must check s->model themselves and fail fast when it's empty. */
int agent_session_init(struct agent_session *s, struct provider *p, const struct hax_opts *opts);

/* Re-resolve the session's model and reasoning effort against the current
 * config and provider `p`, and rebuild the system prompt (its env block
 * embeds the model name). Used after a runtime /provider or /model switch:
 * conversation history, the tools table, and the session log are left
 * intact — only the per-request settings change. Returns 0 on success, -1
 * when no model is available for `p` (logged to stderr). */
int agent_session_reconfigure(struct agent_session *s, struct provider *p);

void agent_session_free(struct agent_session *s);

/* Drop every conversation item, leaving the session ready to start a
 * fresh turn. Preserves model, system prompt, tools, and reasoning_effort
 * — those are session-level config, not per-conversation state. The
 * items vector's capacity is kept allocated so subsequent appends don't
 * have to grow it from zero. Used by `/new` (and `/clear`). */
void agent_session_reset(struct agent_session *s);

/* Snapshot the session's current state into a `struct context` ready
 * to hand to a provider's stream(). The returned struct borrows the
 * session's pointers — its lifetime ends with the next mutating call
 * on the session. */
struct context agent_session_context(const struct agent_session *s);

/* Append ITEM_USER_MESSAGE preceded by ITEM_TURN_BOUNDARY. Used at the
 * start of each user input. */
void agent_session_add_user(struct agent_session *s, const char *text);

/* Append ITEM_TURN_BOUNDARY only — used between consecutive model
 * round-trips inside one user turn (after the first one) so the
 * transcript renderer can mark each round as its own boundary. */
void agent_session_add_boundary(struct agent_session *s);

/* Drain finished items from `t` into the session. Reports:
 *   *out_before        = session's n_items just before the drain
 *   *out_had_tool_call = 1 iff any newly-drained item is a tool call
 * Does NOT call turn_reset — the caller still owns the turn and is
 * expected to reset it after this returns. Both existing callers check
 * t->error before absorbing and then unconditionally turn_reset, so
 * absorb only handles the success case. */
void agent_session_absorb(struct agent_session *s, struct turn *t, size_t *out_before,
                          int *out_had_tool_call);

#endif /* HAX_AGENT_CORE_H */
