/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_CORE_H
#define HAX_AGENT_CORE_H

#include <stddef.h>

#include "provider.h"
#include "tool.h"
#include "turn.h"

struct catalog_split; /* catalog.h — consumers of spend_split include it */

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
    /* Bypass system prompt, Environment section, AGENTS.md, skills, and tools.
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

/* Resolve the `effort` value to send for this session.
 * Empty HAX_EFFORT means "force omit"; unset falls back to
 * the provider default; any non-empty value passes through verbatim. */
const char *resolve_effort(const struct provider *p);

/* Build the malloc'd system prompt from the configured or default base plus
 * agent_env_build_suffix(). Return NULL for raw mode or an explicitly empty
 * HAX_SYSTEM_PROMPT. */
char *build_system_prompt(const char *model_label, int raw);

/* Shared assembly for the stats line shown after each REPL user turn and
 * at the end of a -p run: "42s", "8.9k / 256k (3%)", "$0.042" — one gauge
 * plus fields whose units label themselves (the per-request token detail
 * lives in the transcript's ITEM_TURN_USAGE footers). Fills `segs` in
 * display order (scope order: turn activity, window state, session total)
 * and returns the count (0 when nothing was reported). Unreported fields
 * are skipped rather than rendered as fake zeros: ctx < 0, elapsed_ms < 0,
 * spend <= 0. Callers own the layout — the REPL reflows segments at the
 * " · " seams, oneshot joins them into one stderr line — so what is shown
 * can never drift between the two. spend_approx marks the spend as a
 * catalog estimate rather than a provider-reported charge: "~$0.042". */
#define STATS_SEGS_MAX 3
#define STATS_SEG_LEN  64
int format_stats_segments(char segs[][STATS_SEG_LEN], long ctx, long limit, long elapsed_ms,
                          double spend, int spend_approx);

/* Spend accounting shared by the REPL and the -p path, so the exact-vs-
 * estimated policy lives in exactly one place (like format_stats_segments
 * does for the display side): a response that reports cost is exact and
 * sums into `reported`; one that doesn't is kept as a per-request record,
 * priced against catalog rates when read. */
struct spend_rec {
    struct stream_usage u; /* cost < 0 by construction (else it summed) */
    /* Catalog identity to price against, owned. Stamped at account time
     * so a later /provider or /model switch can't re-rate old requests;
     * NULL = no identity, the record stays unpriceable. */
    char *catalog_id;
    char *model;
};

struct spend_totals {
    double reported; /* provider-reported cost sum, USD */
    /* Records of responses that reported no cost. Priced lazily at read
     * (spend_total) so a late-landing catalog fetch retroactively covers
     * earlier requests, and priced one request at a time so context-tier
     * selection sees each request's own input size. */
    struct spend_rec *recs;
    size_t n_recs;
    size_t cap_recs;
};

/* Account one completed response into `t` — the single definition of the
 * reported-vs-estimated split. catalog_id/model (both may be NULL) stamp
 * the record when the response goes the estimated route. */
void spend_account(struct spend_totals *t, const struct stream_usage *u, const char *catalog_id,
                   const char *model);

/* Total spend for display, USD: reported cost plus the per-record catalog
 * estimates. Sets *approx (when non-NULL) to 1 iff any inexact component
 * exists — an estimate contributed, or a record couldn't be priced at all
 * — so callers can mark the figure ("~$0.42"). */
double spend_total(const struct spend_totals *t, int *approx);

/* True when some record's real usage can't be priced right now (catalog
 * fetch not landed, unknown model, no identity). The oneshot exit path
 * uses it to decide whether draining the in-flight fetch could improve
 * the estimate. */
int spend_unpriced(const struct spend_totals *t);

/* Sum the per-record catalog splits into *out (USD per category; zeroed
 * first) — the estimated portion of the session's spend broken down the
 * same way the transcript footers are. Tier-correct: each record prices
 * at its own request's rates. Returns 1 when at least one record priced
 * (i.e. the split is worth showing), 0 otherwise. Reported-cost
 * responses keep no records, so their charge is deliberately absent
 * here — a reported charge can't be decomposed. */
int spend_split(const struct spend_totals *t, struct catalog_split *out);

void spend_free(struct spend_totals *t);

/* True when the response reported any billing signal (tokens or cost).
 * The gate the error/interrupt paths use before emitting a footer, so a
 * pre-stream failure or a plain Esc cancel doesn't earn a duration-only
 * one — successful round-trips emit unconditionally, usage or not. */
int usage_reported(const struct stream_usage *u);

/* Build the priced payload for an ITEM_TURN_USAGE footer: raw usage plus
 * per-category catalog estimates (see struct turn_usage). Malloc'd, or
 * NULL when there is nothing to show at all (no tokens, no cost, no
 * elapsed time) — a backend that reports no usage still gets a
 * duration-only footer for a successful round-trip. The reported-vs-
 * estimated policy matches spend_account: a reported cost is exact and is
 * never decomposed (the category estimates stay -1); an unreported one
 * gets the catalog estimate, total and split alike, marked
 * cost_estimated. */
struct turn_usage *turn_usage_make(const struct stream_usage *u, long elapsed_ms,
                                   const char *catalog_id, const char *model);

/* Live per-run state shared by the interactive and one-shot paths.
 * Owns the items vector, the assembled system prompt, the tools table,
 * and the resolved model + effort. */
struct agent_session {
    /* Owned copies, not borrowed config_str pointers: a runtime /provider,
     * /model, or /effort commit replaces a whole config tier object (and frees
     * its strings), which would dangle a borrowed pointer the still-live
     * session keeps using. */
    char *model;               /* owned exact id; NULL/"" = no model resolved yet */
    char *model_label;         /* owned display/env label; NULL when model is NULL */
    char *effort;              /* owned; NULL = "omit" */
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
 * config and provider `p`, and rebuild the system prompt (its Environment
 * section embeds the model name). Used after a runtime /provider or /model
 * switch: conversation history, the tools table, and the session log are left
 * intact — only the per-request settings change. Returns 0 on success, -1
 * when no model is available for `p` (logged to stderr). */
int agent_session_reconfigure(struct agent_session *s, struct provider *p);

void agent_session_free(struct agent_session *s);

/* Drop every conversation item, leaving the session ready to start a
 * fresh turn. Preserves model, system prompt, tools, and effort
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

/* Append the round-trip's ITEM_TURN_USAGE footer — the transcript's
 * per-request stats line, priced via turn_usage_make against `p`'s
 * catalog identity and the session model. No-op when `u` reported no
 * usage and no cost. Call once per round-trip, after everything the
 * round-trip put into history (response, tool results, an interrupt
 * marker), so the footer trails the turn it accounts and precedes the
 * next boundary. */
void agent_session_add_turn_usage(struct agent_session *s, const struct provider *p,
                                  const struct stream_usage *u, long elapsed_ms);

/* Record a mid-turn user abort: append INTERRUPT_MARKER as a synthetic
 * assistant message so the next turn and the transcript both see the
 * model was cut short — unless the last content item already carries the
 * marker (a tool result marked at the abort boundary: bash appends its
 * own "[interrupted]" footer when killed by Esc, while read/write/edit
 * return clean results). Inert trailing items (ITEM_TURN_USAGE,
 * ITEM_TURN_BOUNDARY) are looked past when finding that last content
 * item. */
void agent_session_mark_interrupt(struct agent_session *s);

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
