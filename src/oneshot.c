/* SPDX-License-Identifier: MIT */
#include "oneshot.h"

#include <errno.h>
#include <jansson.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "agent_loop.h"
#include "agent_usage.h"
#include "buf.h"
#include "catalog.h"
#include "compact.h"
#include "config.h"
#include "diag.h"
#include "model_meta.h"
#include "provider.h"
#include "session.h"
#include "transcript.h"
#include "system/clock.h"
#include "terminal/ansi.h"
#include "terminal/interrupt.h"
#include "tools/bash_process.h"

/* Bounds unattended agent loops that no supervisor interrupts; this is what max_turns "auto"
 * means in one-shot, and a positive value replaces it. */
#define ONESHOT_DEFAULT_MAX_TURNS 100

/* 128 + SIGINT, the shell convention for an interrupted command. */
#define ONESHOT_EXIT_INTERRUPTED 130

static int resolve_max_turns(void)
{
    int max_turns = config_int("max_turns");
    return max_turns > 0 ? max_turns : ONESHOT_DEFAULT_MAX_TURNS;
}

struct oneshot_state {
    struct provider *provider;
    struct agent_session session;
    struct transcript_log *transcript;
    struct session_log *session_log;
    struct spend_totals spend;
    long started_ms;
    long context_tokens;
    int json;           /* stream conversation records as JSONL on stdout */
    size_t json_cursor; /* session items already streamed */
    int json_errno;     /* first stream-write failure; 0 while the stream is healthy */
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

/* Compaction has no pause seam, so either latched request cancels the retriable transaction;
 * the loop checkpoint that follows turns it into the run's pause or abort. */
static int compact_cancelled(void *user)
{
    (void)user;
    return interrupt_abort_requested() || interrupt_pause_requested();
}

static int compact_context(struct oneshot_state *state)
{
    struct compact_params params = {
        .session = &state->session,
        .provider = state->provider,
        .session_log = state->session_log,
        .transcript_log = state->transcript,
        .hooks = {.user = state,
                  .on_event = account_compaction_event,
                  .tick = compact_cancelled,
                  .is_cancelled = compact_cancelled},
    };
    struct compact_result result;

    compact_run(&params, &result);
    int completed = result.outcome == COMPACT_COMPLETE;
    compact_result_destroy(&result);
    return completed;
}

static void account_turn(const struct agent_loop_turn *turn, void *user)
{
    struct oneshot_state *state = user;
    /* Retried attempts are separate spend records: merging could void an exact terminal
     * charge over their unpriced tokens. */
    agent_spend_account(&state->spend, &turn->usage, state->provider, state->session.model);
    agent_spend_account(&state->spend, &turn->retry_usage, state->provider, state->session.model);
}

static void auto_compact(void *user)
{
    struct oneshot_state *state = user;

    if (!compact_context(state) || state->json)
        return;
    int tty = isatty(fileno(stderr));
    fprintf(stderr, "%s[compacted context]%s\n", tty ? ANSI_DIM : "", tty ? ANSI_RESET : "");
}

/* The --json stream on stdout; docs/sessions.md is the record contract. On a write failure
 * the run stops and exits nonzero rather than keep working unobserved. */

static void json_set_string(json_t *record, const char *key, const char *value)
{
    if (!value || !*value)
        return;
    json_t *string = json_string(value);
    if (string)
        json_object_set_new(record, key, string);
}

static void emit_json_record(struct oneshot_state *state, json_t *record)
{
    if (state->json_errno) {
        json_decref(record);
        return;
    }
    char *text = json_dumps(record, JSON_COMPACT);
    /* Consumers watch the stream live; a record must not sit in a pipe-sized buffer. */
    if (!text || fputs(text, stdout) == EOF || fputc('\n', stdout) == EOF || fflush(stdout) == EOF)
        state->json_errno = errno ? errno : EIO;
    free(text);
    json_decref(record);
}

static void emit_json_items(struct oneshot_state *state)
{
    while (!state->json_errno && state->json_cursor < state->session.n_items)
        emit_json_record(state, item_to_json(&state->session.items[state->json_cursor++]));
}

static void json_turn_begin(void *user)
{
    emit_json_items(user);
}

static void json_tool_seen(const struct item *call, void *user)
{
    (void)call;
    emit_json_items(user);
}

/* Provider wait/chunk hook: a latched abort (first SIGINT/SIGTERM) cancels the in-flight
 * stream; a pause lets it finish and stops at the seam. */
static int loop_tick(void *user)
{
    (void)user;
    return interrupt_abort_requested();
}

/* Seams sample the signal-latched requests and, in --json mode, stream freshly appended
 * records — tool results land mid-batch — without delaying them to the next turn. */
static int loop_checkpoint(void *user)
{
    struct oneshot_state *state = user;

    /* On abort, leave pending records to the post-loop drain: repair may still stamp them
     * (a killed tool's result), and the drain emits what the session file records. */
    if (!interrupt_abort_requested() && state->json) {
        emit_json_items(state);
        /* A dead stream means nobody is supervising the run: stop launching tools. */
        if (state->json_errno)
            return AGENT_LOOP_SIG_ABORT;
    }
    /* Sample abort after the emission too: an interrupt latches both requests, so reading only
     * the pause half of one that landed during a blocking write would misreport it. */
    if (interrupt_abort_requested())
        return AGENT_LOOP_SIG_ABORT;
    if (interrupt_pause_requested())
        return AGENT_LOOP_SIG_PAUSE;
    return AGENT_LOOP_SIG_NONE;
}

/* The run's identity in the file-header schema: the session id when recording, and the live
 * selection, which on a resumed run reflects this run rather than the original header. The
 * consumer knows when it launched the process, so no timestamp is fabricated. */
static void emit_json_session_record(struct oneshot_state *state)
{
    const struct agent_session *session = &state->session;
    const char *preset = config_str("preset");
    char *cwd = getcwd(NULL, 0);
    struct session_header header = {
        .id = session_log_resume_hint(state->session_log),
        .cwd = cwd,
        .provider = agent_provider_id(state->provider),
        .model = session->model,
        .model_label = session->model_label,
        .effort = session->effort,
        .preset = (preset && *preset) ? preset : NULL,
    };
    emit_json_record(state, session_header_to_json(&header));
    free(cwd);
}

static const char *loop_outcome_name(enum agent_loop_outcome outcome)
{
    switch (outcome) {
    case AGENT_LOOP_COMPLETE:
        return "complete";
    case AGENT_LOOP_PROVIDER_ERROR:
        return "error";
    case AGENT_LOOP_INTERRUPTED:
        return "interrupted";
    case AGENT_LOOP_PAUSED:
        return "paused";
    case AGENT_LOOP_MAX_TURNS:
        return "max_turns";
    }
    return "error";
}

/* The final turn's message text as plain -p prints it, minus the enforced trailing newline.
 * NULL when the turn produced no text. */
static char *collect_final_text(const struct item *items, size_t from, size_t to)
{
    struct buf text;
    buf_init(&text);
    for (size_t i = from; i < to; i++) {
        if (items[i].kind != ITEM_ASSISTANT_MESSAGE || !items[i].text || !*items[i].text)
            continue;
        if (text.len > 0 && text.data[text.len - 1] != '\n')
            buf_append(&text, "\n", 1);
        buf_append_str(&text, items[i].text);
    }
    if (text.len == 0) {
        buf_free(&text);
        return NULL;
    }
    return buf_steal(&text);
}

static void emit_json_result(struct oneshot_state *state, const struct agent_loop_result *result,
                             double spend, int spend_estimated)
{
    json_t *record = json_object();

    json_object_set_new(record, "type", json_string("result"));
    json_object_set_new(record, "outcome", json_string(loop_outcome_name(result->outcome)));
    if (result->outcome == AGENT_LOOP_COMPLETE) {
        char *text = collect_final_text(state->session.items, result->final_items_from,
                                        result->final_items_to);
        json_set_string(record, "text", text);
        free(text);
    }
    json_set_string(record, "error", result->error_message);
    json_object_set_new(record, "turns", json_integer(result->turns));
    json_object_set_new(record, "elapsed_ms", json_integer(monotonic_ms() - state->started_ms));
    if (state->context_tokens >= 0)
        json_object_set_new(record, "context_tokens", json_integer(state->context_tokens));
    if (spend > 0) {
        json_object_set_new(record, "cost", json_real(spend));
        if (spend_estimated)
            json_object_set_new(record, "cost_estimated", json_true());
    }
    json_set_string(record, "session_id", session_log_resume_hint(state->session_log));
    emit_json_record(state, record);
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
        /* The banner announces the id before the first provider call, so the file must exist by
         * then: a run killed outright never reaches the exit hint. */
        session_log_begin(state->session_log);
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

static int print_assistant_messages(const struct item *items, size_t from, size_t to)
{
    for (size_t i = from; i < to; i++) {
        if (items[i].kind != ITEM_ASSISTANT_MESSAGE || !items[i].text || !*items[i].text)
            continue;

        size_t text_len = strlen(items[i].text);
        fwrite(items[i].text, 1, text_len, stdout);
        if (items[i].text[text_len - 1] != '\n')
            fputc('\n', stdout);
    }
    return fflush(stdout) == EOF || ferror(stdout) ? -1 : 0;
}

static int handle_loop_result(const struct oneshot_state *state,
                              const struct agent_loop_result *result, int max_turns)
{
    switch (result->outcome) {
    case AGENT_LOOP_COMPLETE:
        if (state->json)
            return 0;
        if (print_assistant_messages(state->session.items, result->final_items_from,
                                     result->final_items_to) < 0) {
            hax_err("cannot write the final answer: %s", strerror(errno));
            return 1;
        }
        return 0;
    case AGENT_LOOP_PROVIDER_ERROR:
        hax_err("provider error: %s",
                result->error_message ? result->error_message : "(no message)");
        return 1;
    case AGENT_LOOP_MAX_TURNS:
        hax_err("max turns (%d) exceeded; aborting", max_turns);
        return 1;
    case AGENT_LOOP_INTERRUPTED:
        /* A dead --json stream also aborts; its diagnostic is the write error reported later. */
        if (!interrupt_abort_requested())
            return 1;
        hax_err("interrupted");
        return ONESHOT_EXIT_INTERRUPTED;
    case AGENT_LOOP_PAUSED:
        hax_err("paused at a turn boundary");
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

static void print_exit_notes(struct oneshot_state *state, double spend, int spend_approx)
{
    const char *resume_hint = session_log_resume_hint(state->session_log);
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

/* Prepare the run up to its first provider call: process-wide signal dispositions, the resolved
 * session (fresh or resumed) with its logs, the run announcement, and the clock. Returns -1
 * after printing a diagnostic; the caller owns state cleanup either way. */
static int start_run(struct oneshot_state *state, const char *prompt,
                     const struct hax_opts *options)
{
    struct provider *provider = state->provider;

    /* Headless mode still needs fatal signals to terminate its spawned process groups. The
     * graceful request handlers are installed only around the run's provider work; a stray
     * pause signal outside that window is ignored rather than left at its process-killing
     * default. */
    interrupt_install_fatal_signal_handlers();
    interrupt_set_fatal_signal_hook(bash_shell_pgids_kill);
    signal(SIGUSR1, SIG_IGN);
    /* A vanished stdout consumer must surface as a checked write error, not as SIGPIPE death,
     * which would skip killing spawned process groups and task cleanup. Tool children reset
     * to the default disposition before exec. */
    signal(SIGPIPE, SIG_IGN);

    /* Effort must reflect the completed startup probe before session initialization. */
    model_meta_wait(provider);
    agent_session_init(&state->session, provider, options);
    if (!state->session.model || !*state->session.model) {
        hax_err("no model selected for provider '%s' — pass --model or set HAX_MODEL",
                provider->name ? provider->name : "?");
        return -1;
    }

    struct session_meta resume_metadata = {0};
    size_t resumed_item_count = 0;
    if (resume_session(state, options->resume_path, &resume_metadata, &resumed_item_count) != 0) {
        session_meta_free(&resume_metadata);
        return -1;
    }
    open_logs(state, options, &resume_metadata, resumed_item_count);
    session_meta_free(&resume_metadata);

    /* Resumed history is context, not this run's events: stream only what the run appends. */
    state->json_cursor = state->session.n_items;
    if (!prompt && agent_session_resume_tail(&state->session) == AGENT_RESUME_TAIL_EMPTY) {
        hax_err("the resumed session has no conversation to continue — pass a prompt");
        return -1;
    }

    /* The stream carries the banner's facts (and later the stats) structurally, so --json emits
     * records in their place; stderr keeps only genuine diagnostics. */
    if (state->json) {
        emit_json_session_record(state);
        /* Bail before the first provider call: with no working stream the run has no value. */
        if (state->json_errno) {
            hax_err("cannot write --json stream: %s", strerror(state->json_errno));
            return -1;
        }
    } else {
        print_start_banner(state, options);
    }

    state->started_ms = monotonic_ms();
    if (provider->catalog_id) {
        long stale_days = catalog_prefetch();
        if (stale_days > 0)
            hax_warn("model catalog last refreshed %ld days ago — cost estimates may be stale",
                     stale_days);
    }
    return 0;
}

/* Shape the empty result of a run stopped before its loop for the shared reporting path. */
static void loop_result_stopped(struct agent_loop_result *loop_result,
                                enum agent_loop_outcome outcome)
{
    memset(loop_result, 0, sizeof(*loop_result));
    loop_result->outcome = outcome;
    loop_result->last_context_tokens = -1;
}

/* Append the user's input — the prompt, or the resume decision a promptless run derives from
 * the recorded tail — and drive the continuation loop over it. */
static void run_user_turn(struct oneshot_state *state, const char *prompt, int max_turns,
                          struct agent_loop_result *loop_result)
{
    int continued = 0;
    if (prompt) {
        agent_session_add_user(&state->session, prompt);
    } else {
        switch (agent_session_resume_tail(&state->session)) {
        case AGENT_RESUME_TAIL_MARKED:
            agent_session_add_continuation(&state->session);
            break;
        case AGENT_RESUME_TAIL_USER:
            break;
        case AGENT_RESUME_TAIL_CLEAN:
        case AGENT_RESUME_TAIL_EMPTY: /* start_run rejected it; a fresh seed reads CLEAN */
            continued = 1;
            break;
        }
    }
    /* Persist the triggering prompt before entering a provider call that may not return. */
    agent_flush_logs(state->transcript, state->session_log, state->session.items,
                     state->session.n_items);
    if (state->json) {
        emit_json_items(state);
        /* A dead stream means nobody is supervising the run: don't launch the provider call.
         * finish_run reports the write error and the nonzero exit. */
        if (state->json_errno) {
            loop_result_stopped(loop_result, AGENT_LOOP_INTERRUPTED);
            return;
        }
    }

    struct agent_loop_params loop_params = {
        .session = &state->session,
        .provider = state->provider,
        .tlog = state->transcript,
        .slog = state->session_log,
        .max_turns = max_turns,
        .continued = continued,
        .hooks =
            {
                .user = state,
                .tick = loop_tick,
                .turn_end = account_turn,
                .checkpoint = loop_checkpoint,
                .compact = auto_compact,
            },
    };
    if (state->json) {
        loop_params.hooks.turn_begin = json_turn_begin;
        loop_params.hooks.tool_seen = json_tool_seen;
    }
    agent_loop_run(&loop_params, loop_result);
}

/* Settle the finished run — records drained, tasks resolved, usage priced — and report its
 * outcome as the answer or the result record. Returns the process exit status. */
static int finish_run(struct oneshot_state *state, const struct agent_loop_result *loop_result,
                      int max_turns)
{
    state->context_tokens = loop_result->last_context_tokens;
    /* The final turn ends with no hook after absorption, so drain its records before task
     * shutdown and the pricing wait below can delay them. */
    if (state->json)
        emit_json_items(state);
    int result = handle_loop_result(state, loop_result, max_turns);
    /* The loop tears down (keepawake) after its last cancellation check, so a stop can land
     * with the outcome already decided. The recorded outcome stands — the work finished — but
     * the exit status must not report success the user cancelled. */
    if (result == 0 && interrupt_abort_requested())
        result = ONESHOT_EXIT_INTERRUPTED;

    agent_finalize_tasks(&state->session, state->transcript, state->session_log);

    /* A short run may finish before the initial catalog fetch can price its usage. */
    if (agent_spend_has_unpriced(&state->spend))
        catalog_drain(3000);
    int spend_approx = 0;
    double spend = agent_spend_total(&state->spend, &spend_approx);
    if (state->json) {
        /* Task finalization may have appended a killed-tasks note after the post-loop drain. */
        emit_json_items(state);
        emit_json_result(state, loop_result, spend, spend_approx);
        /* A terminal Ctrl-C signals the whole pipeline, so after a signal interrupt a dead
         * stream is expected collateral: keep the interrupt's diagnostic and exit status. */
        if (state->json_errno && !interrupt_abort_requested()) {
            hax_err("cannot write --json stream: %s", strerror(state->json_errno));
            result = 1;
        }
    } else {
        print_exit_notes(state, spend, spend_approx);
    }
    return result;
}

int oneshot_run(struct provider *provider, const char *prompt, const struct hax_opts *options)
{
    int max_turns = resolve_max_turns();
    struct oneshot_state state = {
        .provider = provider,
        .context_tokens = -1,
        .json = options->json,
    };

    if (start_run(&state, prompt, options) != 0) {
        oneshot_state_destroy(&state);
        return 1;
    }

    /* The graceful window covers the run's provider work: the resumed compaction pass below and
     * the agent loop. Any earlier, a latch would silently defeat the signal during unbounded
     * startup waits; afterwards the outcome is decided, and shutdown (task finalization, the
     * pricing drain) must not swallow a first Ctrl-C — deliberately even though that makes the
     * final report best-effort under a stop signal. handle_loop_result reads the latch. */
    interrupt_clear_requests();
    interrupt_install_request_signal_handlers();

    /* The loop compacts only at continuation seams it reaches itself, and a pause stops just
     * before that check, so continuing a resumed run owns the pre-request pass — the REPL's
     * deferred compaction before a send. */
    if (options->resume_path &&
        compact_should_auto(agent_session_last_context_tokens(&state.session),
                            model_meta_context(provider, state.session.model)))
        auto_compact(&state);

    struct agent_loop_result loop_result;
    if (interrupt_abort_requested() || interrupt_pause_requested()) {
        /* The stop belongs to the compaction and cancels the whole send, like the REPL's
         * pre-send transaction: nothing is appended and no turn launches. */
        loop_result_stopped(&loop_result, interrupt_abort_requested() ? AGENT_LOOP_INTERRUPTED
                                                                      : AGENT_LOOP_PAUSED);
    } else {
        run_user_turn(&state, prompt, max_turns, &loop_result);
    }
    interrupt_install_fatal_signal_handlers();
    signal(SIGUSR1, SIG_IGN);

    int result = finish_run(&state, &loop_result, max_turns);
    agent_loop_result_destroy(&loop_result);
    oneshot_state_destroy(&state);
    return result;
}
