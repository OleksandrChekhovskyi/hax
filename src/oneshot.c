/* SPDX-License-Identifier: MIT */
#include "oneshot.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "agent_loop.h"
#include "agent_usage.h"
#include "catalog.h"
#include "compact.h"
#include "config.h"
#include "model_meta.h"
#include "provider.h"
#include "session.h"
#include "transcript.h"
#include "util.h"
#include "terminal/ansi.h"
#include "terminal/interrupt.h"
#include "tools/bash_process.h"

struct oneshot_state {
    struct provider *provider;
    struct agent_session session;
    struct transcript_log *transcript;
    struct session_log *session_log;
    struct spend_totals spend;
    long started_ms;
    long context_tokens;
    int json_events;
};

static int account_compaction_event(const struct stream_event *event, void *user)
{
    struct oneshot_state *state = user;
    const struct stream_usage *usage = NULL;

    if (event->kind == EV_DONE)
        usage = &event->u.done.usage;
    else if (event->kind == EV_ERROR)
        usage = event->u.error.usage;
    else if (event->kind == EV_RETRY)
        usage = event->u.retry.usage;
    if (usage)
        agent_spend_account(&state->spend, usage, state->provider, state->session.model);
    return 0;
}

static int compact_context(struct oneshot_state *state)
{
    struct compact_params params = {
        .session = &state->session,
        .provider = state->provider,
        .session_log = state->session_log,
        .transcript_log = state->transcript,
        .hooks = {.user = state, .on_event = account_compaction_event},
    };
    struct compact_result result;

    compact_run(&params, &result);
    int completed = result.outcome == COMPACT_COMPLETE;
    compact_result_destroy(&result);
    return completed;
}

/* Emit one progress event as a JSON line on stdout. Ownership of `event` passes in;
 * a write failure is silently ignored — progress is advisory, never fatal. */
static void json_emit(json_t *event)
{
    char *line = json_dumps(event, JSON_COMPACT | JSON_ENSURE_ASCII);
    if (line) {
        fputs(line, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        free(line);
    }
    json_decref(event);
}

/* Attach usage fields to `object` under "usage"; unknown (-1) counts are omitted. */
static void json_add_usage(json_t *object, const struct stream_usage *usage)
{
    json_t *u = json_object();
    long total = 0;

    if (usage->input_tokens >= 0) {
        json_object_set_new(u, "input_tokens", json_integer(usage->input_tokens));
        total += usage->input_tokens;
    }
    if (usage->output_tokens >= 0) {
        json_object_set_new(u, "output_tokens", json_integer(usage->output_tokens));
        total += usage->output_tokens;
    }
    if (usage->cached_tokens >= 0)
        json_object_set_new(u, "cached_tokens", json_integer(usage->cached_tokens));
    if (total > 0)
        json_object_set_new(u, "totalTokens", json_integer(total));
    json_object_set_new(object, "usage", u);
}

static void account_turn(const struct agent_loop_turn *turn, void *user)
{
    struct oneshot_state *state = user;
    /* Retried attempts are separate spend records: merging could void an exact terminal
     * charge over their unpriced tokens. */
    agent_spend_account(&state->spend, &turn->usage, state->provider, state->session.model);
    agent_spend_account(&state->spend, &turn->retry_usage, state->provider, state->session.model);

    if (!state->json_events)
        return;
    json_t *event = json_object();
    json_object_set_new(event, "type", json_string("turn_end"));
    if (turn->served_model)
        json_object_set_new(event, "model", json_string(turn->served_model));
    if (turn->elapsed_ms >= 0)
        json_object_set_new(event, "elapsed_ms", json_integer(turn->elapsed_ms));
    json_add_usage(event, &turn->usage);
    json_emit(event);
}

static void oneshot_turn_begin(void *user)
{
    struct oneshot_state *state = user;

    if (!state->json_events)
        return;
    json_emit(json_pack("{s:s}", "type", "turn_start"));
}

static void oneshot_tool_seen(const struct item *call, void *user)
{
    struct oneshot_state *state = user;

    if (!state->json_events || !call->tool_name)
        return;
    /* Arguments ride as a string, not embedded JSON: a malformed argument blob must
     * not corrupt the event line. */
    json_t *event = json_pack("{s:s}", "type", "tool_use");
    json_object_set_new(event, "name", json_string(call->tool_name));
    if (call->tool_arguments_json)
        json_object_set_new(event, "arguments", json_string(call->tool_arguments_json));
    json_emit(event);
}

static void auto_compact(void *user)
{
    struct oneshot_state *state = user;

    if (!compact_context(state))
        return;
    int tty = isatty(fileno(stderr));
    fprintf(stderr, "%s[compacted context]%s\n", tty ? ANSI_DIM : "", tty ? ANSI_RESET : "");
}

static int resume_session(struct oneshot_state *state, const char *path,
                          struct session_meta *metadata, size_t *item_count)
{
    if (!path)
        return 0;

    struct item *items = NULL;
    size_t count = 0;
    if (session_load(path, &items, &count, metadata) != 0) {
        hax_err("could not resume session '%s'", path);
        return -1;
    }

    state->session.items = items;
    state->session.n_items = count;
    state->session.cap_items = count;
    *item_count = count;
    return 0;
}

static void open_logs(struct oneshot_state *state, const struct hax_opts *options,
                      const struct session_meta *resume_metadata, size_t resumed_item_count)
{
    struct agent_session *session = &state->session;
    struct provider *provider = state->provider;

    state->transcript =
        transcript_log_open(session->system_prompt, session->tools, session->n_tools);
    if (agent_recording_enabled(provider)) {
        state->session_log =
            options->resume_path
                ? session_log_resume(options->resume_path, resume_metadata->provider,
                                     resume_metadata->model, resume_metadata->effort,
                                     resume_metadata->preset, resumed_item_count)
                : session_log_open(agent_provider_id(provider), session->model,
                                   session->model_label, session->effort, config_str("preset"));
    }
    if (options->resume_path)
        session_log_set_meta(state->session_log, agent_provider_id(provider), session->model,
                             session->model_label, session->effort, config_str("preset"));
    if (resumed_item_count > 0)
        transcript_log_append(state->transcript, session->items, session->n_items);
}

static void print_start_banner(const struct oneshot_state *state, const struct hax_opts *options)
{
    const struct agent_session *session = &state->session;
    const char *provider_name = state->provider->name ? state->provider->name : "?";
    const char *model_label = session->model_label ? session->model_label : session->model;
    const char *preset = config_str("preset");
    int tty = isatty(fileno(stderr));

    /* Provenance remains useful in redirected diagnostics, so only the styling is TTY-gated. */
    if (preset && *preset)
        fprintf(stderr, "%shax [%s]: %s · %s", tty ? ANSI_DIM : "", preset, provider_name,
                model_label);
    else
        fprintf(stderr, "%shax: %s · %s", tty ? ANSI_DIM : "", provider_name, model_label);
    if (session->effort)
        fprintf(stderr, " · %s", session->effort);
    if (options->provider_autoselected)
        fprintf(stderr, " (auto-selected)");
    else if (options->resume_path)
        fprintf(stderr, " (resumed)");

    const char *session_id = session_log_resume_hint(state->session_log);
    if (session_id)
        fprintf(stderr, " · session %s", session_id);
    fprintf(stderr, "%s\n\n", tty ? ANSI_RESET : "");
}

static void print_assistant_messages(const struct item *items, size_t from, size_t to)
{
    for (size_t i = from; i < to; i++) {
        if (items[i].kind != ITEM_ASSISTANT_MESSAGE || !items[i].text || !*items[i].text)
            continue;

        size_t text_len = strlen(items[i].text);
        fwrite(items[i].text, 1, text_len, stdout);
        if (items[i].text[text_len - 1] != '\n')
            fputc('\n', stdout);
    }
}

/* Concatenate the final assistant messages the way the text path prints them,
 * returning an owned string (possibly empty) for the result event. */
static char *final_messages_text(const struct item *items, size_t from, size_t to)
{
    size_t cap = 0;

    for (size_t i = from; i < to; i++) {
        if (items[i].kind != ITEM_ASSISTANT_MESSAGE || !items[i].text || !*items[i].text)
            continue;
        cap += strlen(items[i].text) + 2;
    }
    char *text = xmalloc(cap + 1);
    char *cursor = text;

    for (size_t i = from; i < to; i++) {
        if (items[i].kind != ITEM_ASSISTANT_MESSAGE || !items[i].text || !*items[i].text)
            continue;
        size_t len = strlen(items[i].text);
        memcpy(cursor, items[i].text, len);
        cursor += len;
        if (items[i].text[len - 1] != '\n')
            *cursor++ = '\n';
    }
    *cursor = '\0';
    return text;
}

static int handle_loop_result(const struct oneshot_state *state,
                              const struct agent_loop_result *result, int max_turns)
{
    switch (result->outcome) {
    case AGENT_LOOP_COMPLETE:
        if (state->json_events) {
            char *text = final_messages_text(state->session.items, result->final_items_from,
                                             result->final_items_to);
            json_emit(json_pack("{s:s,s:s*}", "type", "result", "text", text));
            free(text);
        } else {
            print_assistant_messages(state->session.items, result->final_items_from,
                                     result->final_items_to);
        }
        return 0;
    case AGENT_LOOP_PROVIDER_ERROR:
        if (state->json_events)
            json_emit(json_pack("{s:s,s:s*}", "type", "error", "message",
                                result->error_message ? result->error_message : ""));
        hax_err("provider error: %s",
                result->error_message ? result->error_message : "(no message)");
        return 1;
    case AGENT_LOOP_MAX_TURNS:
        if (state->json_events) {
            char *message = xasprintf("max turns (%d) exceeded", max_turns);

            json_emit(json_pack("{s:s,s:s*}", "type", "error", "message", message));
            free(message);
        }
        hax_err("max turns (%d) exceeded; aborting", max_turns);
        return 1;
    case AGENT_LOOP_INTERRUPTED:
    case AGENT_LOOP_PAUSED:
        if (state->json_events)
            json_emit(json_pack("{s:s}", "type", "interrupted"));
        return 1;
    }
    return 1;
}

static void print_stats_line(const struct oneshot_state *state, double spend, int spend_approx,
                             int tty)
{
    char segments[AGENT_STATS_MAX_SEGMENTS][AGENT_STATS_SEGMENT_LEN];
    int segment_count = agent_format_stats_segments(
        segments, state->context_tokens, model_meta_context(state->provider, state->session.model),
        monotonic_ms() - state->started_ms, spend, spend_approx);

    fputs(tty ? ANSI_DIM : "", stderr);
    for (int i = 0; i < segment_count; i++)
        fprintf(stderr, "%s%s", i ? " · " : "", segments[i]);
    fprintf(stderr, "%s\n", tty ? ANSI_RESET : "");
}

static void print_exit_notes(struct oneshot_state *state)
{
    const char *resume_hint = session_log_resume_hint(state->session_log);

    /* A short run may finish before the initial catalog fetch can price its usage. */
    if (agent_spend_has_unpriced(&state->spend))
        catalog_drain(3000);

    int spend_approx = 0;
    double spend = agent_spend_total(&state->spend, &spend_approx);
    int have_stats = state->context_tokens >= 0 || spend > 0;
    if (!resume_hint && !have_stats)
        return;

    /* Preserve answer-before-diagnostics ordering when stdout and stderr share a destination. */
    fflush(stdout);
    int tty = isatty(fileno(stderr));
    fputc('\n', stderr);
    if (have_stats)
        print_stats_line(state, spend, spend_approx, tty);
    if (resume_hint)
        fprintf(stderr, "%sresume with: hax --resume=%s%s\n", tty ? ANSI_DIM : "", resume_hint,
                tty ? ANSI_RESET : "");
}

static void oneshot_state_destroy(struct oneshot_state *state)
{
    transcript_log_close(state->transcript);
    session_log_close(state->session_log);
    agent_spend_free(&state->spend);
    agent_session_free(&state->session);
}

int oneshot_run(struct provider *provider, const char *prompt, const struct hax_opts *options,
                int max_turns)
{
    struct oneshot_state state = {
        .provider = provider,
        .context_tokens = -1,
        .json_events = options->json_events,
    };

    /* Headless mode still needs fatal signals to terminate its spawned process groups. */
    interrupt_install_fatal_signal_handlers();
    interrupt_set_fatal_signal_hook(bash_shell_pgids_kill);

    /* Effort must reflect the completed startup probe before session initialization. */
    model_meta_wait(provider);
    agent_session_init(&state.session, provider, options);
    if (!state.session.model || !*state.session.model) {
        hax_err("no model selected for provider '%s' — pass --model or set HAX_MODEL",
                provider->name ? provider->name : "?");
        oneshot_state_destroy(&state);
        return 1;
    }

    struct session_meta resume_metadata = {0};
    size_t resumed_item_count = 0;
    if (resume_session(&state, options->resume_path, &resume_metadata, &resumed_item_count) != 0) {
        session_meta_free(&resume_metadata);
        oneshot_state_destroy(&state);
        return 1;
    }

    open_logs(&state, options, &resume_metadata, resumed_item_count);
    session_meta_free(&resume_metadata);

    agent_session_add_user(&state.session, prompt);
    /* Persist the triggering prompt before entering a provider call that may not return. */
    agent_flush_logs(state.transcript, state.session_log, state.session.items,
                     state.session.n_items);
    print_start_banner(&state, options);

    state.started_ms = monotonic_ms();
    if (provider->catalog_id) {
        long stale_days = catalog_prefetch();
        if (stale_days > 0)
            hax_warn("model catalog last refreshed %ld days ago — cost estimates may be stale",
                     stale_days);
    }

    struct agent_loop_params loop_params = {
        .session = &state.session,
        .provider = provider,
        .tlog = state.transcript,
        .slog = state.session_log,
        .max_turns = max_turns,
        .hooks =
            {
                .user = &state,
                .turn_begin = oneshot_turn_begin,
                .turn_end = account_turn,
                .tool_seen = oneshot_tool_seen,
                .compact = auto_compact,
            },
    };
    struct agent_loop_result loop_result;
    agent_loop_run(&loop_params, &loop_result);
    state.context_tokens = loop_result.last_context_tokens;
    int result = handle_loop_result(&state, &loop_result, max_turns);
    agent_loop_result_destroy(&loop_result);

    agent_finalize_tasks(&state.session, state.transcript, state.session_log);
    print_exit_notes(&state);
    oneshot_state_destroy(&state);
    return result;
}
