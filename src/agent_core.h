/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_CORE_H
#define HAX_AGENT_CORE_H

#include <stddef.h>

#include "provider.h"
#include "tool.h"

/* State and conversation-record bookkeeping shared by the interactive and one-shot frontends:
 * the canonical flat item log (struct agent_session) with its views and mutators, and the
 * helpers that keep the record consistent with its logs and background tasks. Anything here
 * must behave identically in both frontends and needs no live provider stream — provider
 * round-trips belong in agent_loop, presentation in the frontends. */

/* Synthetic history content used to keep interrupted or refused tool turns well formed. */
#define INTERRUPT_MARKER "[interrupted]"
#define REFUSED_RESULT   "error: tool calls are disabled in this session"
#define REFUSED_MARKER   "[refused: --raw, no tools advertised]"

/* Synthetic user text for an empty-send resume. Origin, not text, identifies continuations. */
#define CONTINUE_MARKER "[continue]"

struct hax_opts {
    int raw;                   /* send only user content and advertise no tools */
    const char *resume_path;   /* borrowed session path; NULL starts a new session */
    int provider_autoselected; /* show the one-shot provider-selection banner */
};

const struct tool *agent_find_tool(const char *name);

/* Return the configured provider id, or the live provider's borrowed name when unset. */
const char *agent_provider_id(const struct provider *provider);

/* Return agent_provider_id(), substituting "none" when no provider is selected. */
const char *agent_provider_log_name(const struct provider *provider);

/* Honor `no_session`; auto mode disables recording only for internal providers. */
int agent_recording_enabled(const struct provider *provider);

/* State shared by the interactive and one-shot frontends. String and vector fields are owned
 * except for provider_id, which remains valid only while the producing provider is alive. */
struct agent_session {
    char *model;       /* exact model id; NULL/empty when unresolved */
    char *model_label; /* display/environment label; NULL when model is NULL */
    char *effort;      /* NULL omits reasoning effort */
    /* Stable id stamped into reasoning provenance; never the configurable display name. */
    const char *provider_id;
    char *system_prompt;
    struct tool_def *tools;
    size_t n_tools;
    size_t cap_tools;
    /* Per-entry ownership: a host-registered def owns its strings, a built-in points at static
     * storage. Parallel to `tools` because a host may replace a built-in at any index. NULL
     * means no entry is owned, so a hand-assembled session needs only `tools` and `n_tools`. */
    unsigned char *tools_owned;
    int raw_mode;

    /* The whole conversation, including prefixes a compaction has already summarized. */
    struct item *items;
    size_t n_items;
    size_t cap_items;
};

/* Initialize a session. A missing model is valid so the interactive frontend can prompt for one. */
void agent_session_init(struct agent_session *session, struct provider *provider,
                        const struct hax_opts *opts);

/* Advertise a host-provided tool alongside the built-ins, deep-copying `def` so the caller keeps
 * ownership of everything it passed. A name that already appears replaces that entry, which is
 * how a host redefines a built-in rather than adding a second tool under one name. Returns -1 in
 * raw mode, which advertises no tools at all. Dispatch is a separate concern: registering a def
 * tells the model the tool exists but does not decide what runs when it is called. */
int agent_session_add_tool(struct agent_session *session, const struct tool_def *def);

/* Re-resolve request settings for `provider` without changing history or tools. Returns -1 when
 * the provider has no configured or default model; the existing settings remain unchanged. */
int agent_session_reconfigure(struct agent_session *session, struct provider *provider);

/* Wait for model metadata and update cached effort. Returns true if it changed. `previous`, when
 * non-NULL, receives ownership of the replaced value; otherwise the old value is freed. */
int agent_session_resync_effort(struct agent_session *session, struct provider *provider,
                                char **previous);

void agent_session_free(struct agent_session *session);

/* Clear conversation items while preserving session settings and item-vector capacity. */
void agent_session_reset(struct agent_session *session);

/* Return a borrowed provider context, valid until the next session mutation. Items before the
 * newest compaction seed are excluded: compaction summarizes a prefix rather than discarding it,
 * so this is the only view that answers what the model sees. */
struct context agent_session_context(const struct agent_session *session);

/* Transfer ownership of `item` into the session. */
void agent_session_append(struct agent_session *session, struct item item);

/* Append a turn boundary followed by a copied user message. */
void agent_session_add_user(struct agent_session *session, const char *text);

/* Append the synthetic user turn used to resume an interrupted response. */
void agent_session_add_continuation(struct agent_session *session);

/* Append a boundary between provider round-trips in one user turn. */
void agent_session_add_boundary(struct agent_session *session);

/* Append an owned usage footer for one provider round-trip. `response` is the identity the
 * provider reported for it, or NULL when the footer stands in for no single stream. */
void agent_session_add_turn_usage(struct agent_session *session, const struct provider *provider,
                                  const struct stream_usage *usage, long elapsed_ms,
                                  const struct stream_response *response);

/* Add an interrupt marker unless the latest content is an already-marked tool result. */
void agent_session_mark_interrupt(struct agent_session *session);

struct turn;

struct agent_absorb_result {
    size_t items_from;
    int had_tool_call;
};

/* Transfer completed turn items into the session. The caller still owns and resets `turn`. */
struct agent_absorb_result agent_session_absorb(struct agent_session *session, struct turn *turn);

struct transcript_log;
struct session_log;

/* Record items both logs have not seen yet; NULL-safe on either log. */
void agent_flush_logs(struct transcript_log *tlog, struct session_log *slog,
                      const struct item *items, size_t n_items);

/* Resolve every uncollected background task into the conversation record — final status for
 * finished ones, killed-at-exit for the rest — flush the logs, and kill whatever still runs.
 * Tasks belong to the conversation that started them: call this before its record is replaced
 * or closed, so it never dangles on a "[detached as task ...]" report or advertises output the
 * shutdown destroys, and no completion is announced into an unrelated conversation. */
void agent_finalize_tasks(struct agent_session *session, struct transcript_log *tlog,
                          struct session_log *slog);

#endif /* HAX_AGENT_CORE_H */
