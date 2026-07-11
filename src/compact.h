/* SPDX-License-Identifier: MIT */
#ifndef HAX_COMPACT_H
#define HAX_COMPACT_H

#include <stddef.h>

#include "provider.h" /* stream_cb, http_tick_cb */

struct item;
struct agent_session;
struct turn;
struct session_log;
struct transcript_log;

/*
 * Conversation compaction: replace a long history with a model-generated
 * structured summary so the session can keep going without overflowing the
 * context window.
 *
 * The shared mechanism lives here: the prompt/seed text, the auto-trigger
 * threshold logic, the context-window resolver, and the two orchestration
 * steps both entry points run — compact_summarize (stream the summary) and
 * compact_apply (swap history + rotate logs). What stays in each entry point
 * is only the part that genuinely differs: agent.c (agent_compact) wraps the
 * stream with a spinner, Esc handling, and a dim notice; oneshot.c runs it
 * silently. The split keeps the history-rebuild policy in one place so the
 * interactive and one-shot paths can't drift.
 */

/* The summarization instruction. Appended as a synthetic trailing user
 * message to the live conversation when generating a summary. Tools stay
 * advertised on the request (to keep the prefix cache warm), so the prompt
 * leans on a "text only, do not call tools" framing; a model that ignores it
 * is handled by the reject-and-retry loop in compact_summarize. */
extern const char *const COMPACT_PROMPT;

/* Preamble prepended to the model's summary to form the single user
 * message that replaces the compacted history. Frames the summary as
 * prior context for the model to continue from. */
extern const char *const COMPACT_SEED_PREAMBLE;

/* Whether auto-compaction is enabled (config key compact.auto). Manual
 * /compact ignores this — it always runs. */
int compact_auto_enabled(void);

/* Pure threshold test: is `ctx_tokens` at or past `pct`% of `limit`?
 * Returns 0 when limit <= 0 (unknown window) or ctx_tokens < 0. */
int compact_over_threshold(long ctx_tokens, long limit, int pct);

/* Decide whether auto-compaction should fire after a (stream) turn, given
 * the last reported context token count and the resolved context-window
 * limit. Honors compact.auto and compact.threshold (percent). Returns 0 when
 * disabled, when the limit is unknown (<= 0), or when ctx is below the
 * threshold. */
int compact_should_auto(long ctx_tokens, long limit);

/* Resolve the context-window size used for the auto-compaction threshold and
 * the per-user-turn "%" display: HAX_CONTEXT_LIMIT (manual override) wins,
 * else the provider's auto-detected probe value, else the model-catalog
 * entry for `model` (providers with a catalog_id but no probe of their own —
 * openai, anthropic), else 0 ("unknown" — both the percentage and
 * auto-compaction are then disabled). `model` may be NULL (skips the
 * catalog tier). */
long compact_context_limit(const struct provider *p, const char *model);

/* Build the summarization instruction sent as the synthetic trailing user
 * message. `instructions` (optional) is appended as extra focus. Caller
 * frees. */
char *compact_build_prompt(const char *instructions);

/* Wrap a model-produced `summary` in the seed-message preamble to form the
 * single user message that replaces compacted history. Caller frees. */
char *compact_build_seed(const char *summary);

/* Pull the summary text out of an assembled turn: the last non-empty
 * assistant message in `items[0..n)`. Returns a fresh copy (caller frees) or
 * NULL when the turn produced no usable text. */
char *compact_extract_summary(const struct item *items, size_t n);

/* Stream a summarization request and return the summary text (caller frees),
 * or NULL on stream error / no usable output.
 *
 * Shared by the interactive and one-shot paths; each supplies its own event
 * sink (`cb`/`cb_user` — which MUST feed `t` via turn_on_event, and may also
 * capture usage/errors and drive a spinner) and `tick`/`tick_user` (the
 * interactive path passes its cancel+idle tick; one-shot passes NULL). The
 * request is the live history plus a synthetic trailing user message with the
 * prompt; the session's tools stay advertised (cache-warm) and the prompt asks
 * for text only. If the model answers with a tool call instead, the call is
 * rejected (never executed) and the request re-streamed, capped at a few
 * attempts. On a returned non-NULL summary the turn's items have been drained;
 * on a stream-error NULL they have not — the caller calls turn_reset(t) either
 * way. Does not touch history: the caller decides (after checking its own
 * cancel state) whether to compact_apply the result.
 *
 * `attempts`, when non-NULL, is incremented once per model stream attempt
 * (the rejected-tool-call retries included), regardless of outcome. This is
 * the request count a stats-keeping caller needs: a user-cancelled attempt
 * emits no terminal event, so the caller's sink alone can't see it. */
char *compact_summarize(const struct agent_session *s, struct provider *p, const char *instructions,
                        struct turn *t, stream_cb cb, void *cb_user, http_tick_cb tick,
                        void *tick_user, int *attempts);

/* Replace the session's history with a single seed user message holding the
 * summary (flagged compact_seed so display and session tooling can tell it
 * from a typed prompt), then rotate both logs to fresh files (a seeded
 * `/new`: the old records stay on disk) and drop now-unreachable bash temp
 * files. Centralizes the history-rebuild policy so the interactive and
 * one-shot paths can't drift. `slog`/`tlog` may be NULL (persistence
 * disabled). */
void compact_apply(struct agent_session *s, struct session_log *slog, struct transcript_log *tlog,
                   const char *summary);

#endif /* HAX_COMPACT_H */
