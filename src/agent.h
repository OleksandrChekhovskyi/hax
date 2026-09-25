/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_H
#define HAX_AGENT_H

/* Interactive frontend for running and mutating an agent REPL session. */

#include "agent_core.h"
#include "provider.h"

struct transcript_log;
struct session_log;
struct render_ctx;
struct loop_schedule;

/* Why an incomplete user turn can be resumed from the prompt. */
enum agent_resume_reason {
    AGENT_RESUME_NONE = 0,
    AGENT_RESUME_PAUSED,
    AGENT_RESUME_MAX_TURNS,
    AGENT_RESUME_INTERRUPTED,
    AGENT_RESUME_ERROR,
};

/* Live interactive state shared by the REPL and its command handlers. */
struct agent_state {
    struct agent_session *session;
    struct provider *provider; /* owned live provider; replacement destroys the previous one */
    struct transcript_log *transcript;
    struct session_log *session_log; /* NULL when session recording is disabled or unavailable */
    struct render_ctx *render;
    struct loop_schedule *loops; /* owned by the interactive frontend */
    char *pending_recall;        /* owned prompt to add to editor recall after a slash command */
    char *pending_preseed;       /* owned text to seed into the next prompt */
    enum agent_resume_reason resume_reason;
    int compaction_deferred; /* settle before appending the next prompt */
};

/* Run the interactive REPL. A provider switch destroys the old provider and updates *provider_io.
 * The caller retains ownership of the provider left in *provider_io on return. */
int agent_run(struct provider **provider_io, const struct hax_opts *options);

/* Reset conversation history, logs, tracked temporary files, and per-conversation statistics.
 * Background tasks are resolved in the outgoing record and stopped. */
void agent_new_conversation(struct agent_state *state);

/* Replace the live conversation with `path` and continue recording there, resolving and
 * stopping the current conversation's background tasks. A load failure is reported and leaves
 * the current conversation unchanged, its tasks still running. */
void agent_resume_session(struct agent_state *state, const char *path);

/* Count typed user prompts; synthetic compaction and continuation messages are excluded. */
size_t agent_user_turn_count(const struct agent_session *session);

/* Return borrowed prompt text for a zero-based user turn, or NULL when out of range. */
const char *agent_user_turn_text(const struct agent_session *session, size_t turn_index);

/* Revert history and logs to before `turn_index`, staging its prompt for editor recall. The caller
 * must pass an existing user turn. A log truncation failure leaves the conversation unchanged. */
void agent_undo(struct agent_state *state, size_t turn_index);

/* Branch before `turn_index`; an index equal to the turn count clones the whole conversation.
 * Requires a materialized session log. Failure leaves the live conversation and original branch
 * unchanged. */
void agent_fork(struct agent_state *state, size_t turn_index);

/* Re-resolve the session against `provider`. A distinct provider transfers into `state` only on
 * success; on failure the caller retains it and the live session and provider remain unchanged.
 * When `announce` is zero, no confirmation is rendered. Returns 0 on success. */
int agent_apply_settings(struct agent_state *state, struct provider *provider, int announce);

/* Rebuild display settings between provider streams. */
void agent_display_refresh(struct agent_state *state);

/* Replace history with a streamed summary. `instructions` may be NULL. Automatic calls suppress
 * no-op notices. Returns 1 only when history was replaced. */
int agent_compact(struct agent_state *state, const char *instructions, int automatic);

#endif /* HAX_AGENT_H */
