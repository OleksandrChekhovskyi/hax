/* SPDX-License-Identifier: MIT */
#include "agent.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "agent_dispatch.h"
#include "agent_loop.h"
#include "catalog.h"
#include "model_meta.h"
#include "compact.h"
#include "config.h"
#include "file_mention.h"
#include "history.h"
#include "paste_image.h"
#include "select.h"
#include "session.h"
#include "slash.h"
#include "tool.h"
#include "transcript.h"
#include "util.h"
#include "render/disp.h"
#include "render/markdown.h"
#include "render/render_ctx.h"
#include "render/spinner.h"
#include "system/spawn.h"
#include "system/tempfiles.h"
#include "terminal/ansi.h"
#include "tools/bash_process.h"
#include "tools/task_registry.h"
#include "terminal/input.h"
#include "terminal/interrupt.h"
#include "terminal/notify.h"
#include "terminal/theme.h"
#include "terminal/ui.h"
#include "terminal/vt_resolve.h"

/* Use ASCII unless wcwidth() can measure the themed UTF-8 glyph correctly. */
static const char *build_prompt(char *buffer, size_t size)
{
    if (locale_have_utf8())
        snprintf(buffer, size, "%s" ANSI_BOLD "❯" ANSI_BOLD_OFF "%s ", theme_open(THEME_ACCENT),
                 theme_close(THEME_ACCENT));
    else
        snprintf(buffer, size, ANSI_BOLD ">" ANSI_BOLD_OFF " ");
    return buffer;
}

/* Per-tool slots need static registry names; compaction can free item-owned names.
 * Unknown names count only toward the total. */
static void stats_count_tool_call(struct session_stats *stats, const char *tool_name)
{
    stats->tool_calls++;
    const struct tool *tool = tool_name ? agent_find_tool(tool_name) : NULL;
    if (!tool)
        return;
    for (size_t i = 0; i < SESSION_STATS_MAX_TOOLS; i++) {
        if (stats->tools[i].name == tool->def.name) {
            stats->tools[i].count++;
            return;
        }
        if (!stats->tools[i].name) {
            stats->tools[i].name = tool->def.name;
            stats->tools[i].count = 1;
            return;
        }
    }
}

/* Tables buffer invisibly until layout completes; delay the spinner to avoid flicker on fast
 * tables. */
#define TABLE_IDLE_TIMEOUT_MS 1500

/* Raw ANSI must bypass disp bookkeeping; otherwise an escape after a buffered newline commits
 * it and leaks an extra blank line at the next block separator. */
static void md_emit_to_disp(const char *bytes, size_t byte_count, int is_raw, void *user)
{
    struct disp *disp = user;
    if (is_raw)
        fwrite(bytes, 1, byte_count, disp_sink(disp));
    else
        disp_write(disp, bytes, byte_count);
}

static int markdown_enabled(void)
{
    /* Cursor-addressed Markdown must never leak into piped output. */
    if (!isatty(fileno(stdout)))
        return 0;
    return config_bool("markdown");
}

static int reasoning_visible(void)
{
    return config_bool("show_reasoning");
}

void agent_display_refresh(struct agent_state *state)
{
    theme_init();
    struct render_ctx *render = state->render;
    render->show_reasoning = reasoning_visible();
    if (render->md) {
        md_free(render->md);
        render->md = NULL;
    }
    if (markdown_enabled())
        render->md = md_new(md_emit_to_disp, &render->disp, md_cols());
}

double agent_session_spend(const struct session_stats *stats, int *estimated)
{
    return agent_spend_total(&stats->spend, estimated);
}

/* Ordinary turns and compaction account request counts and window snapshots differently. */
static void stats_account_usage(struct session_stats *stats, const struct stream_usage *usage,
                                const struct provider *provider, const char *model)
{
    if (usage->input_tokens >= 0)
        stats->input_tokens += usage->input_tokens;
    if (usage->output_tokens >= 0)
        stats->output_tokens += usage->output_tokens;
    if (usage->cached_tokens > 0)
        stats->cached_tokens += usage->cached_tokens;
    if (usage->cache_write_tokens > 0)
        stats->cache_write_tokens += usage->cache_write_tokens;
    if (usage->input_tokens > 0)
        stats->uncached_input_tokens += agent_usage_uncached_input(usage, provider, model);
    agent_spend_account(&stats->spend, usage, provider, model);
}

/* Show one summary per user turn, using the latest context snapshot and cumulative session
 * spend. Wrap only at segment boundaries to keep values intact. */
static void display_stats_line(struct render_ctx *render, const struct provider *provider,
                               const char *model, long context_tokens, long elapsed_ms,
                               const struct session_stats *stats)
{
    int estimated = 0;
    double spend = agent_session_spend(stats, &estimated);
    char segments[AGENT_STATS_MAX_SEGMENTS][AGENT_STATS_SEGMENT_LEN];
    int segment_count =
        agent_format_stats_segments(segments, context_tokens, model_meta_context(provider, model),
                                    elapsed_ms, spend, estimated);
    if (segment_count == 0)
        return;

    struct disp *disp = &render->disp;
    render_open_block(render);
    disp_raw(disp, ANSI_DIM);

    /* Segment bytes equal columns; the separator occupies three display columns. */
    int width = display_width();
    int column = 0;
    for (int i = 0; i < segment_count; i++) {
        int segment_width = (int)strlen(segments[i]);
        if (column > 0) {
            if (column + 3 + segment_width > width) {
                disp_putc(disp, '\n');
                column = 0;
            } else {
                disp_printf(disp, " · ");
                column += 3;
            }
        }
        disp_printf(disp, "%s", segments[i]);
        column += segment_width;
    }
    disp_raw(disp, ANSI_RESET);
    disp_putc(disp, '\n');
    disp_flush(disp);
}

/* Curl wait/chunk hook for cancellation and timer-driven retry/table labels. Ordinary text
 * stalls intentionally show no spinner. */
static int agent_stream_tick(void *user)
{
    struct render_ctx *render = user;
    /* Confirm a soft pause immediately; the idempotent update also overrides pending labels. */
    if (interrupt_soft_requested() && !interrupt_requested()) {
        spinner_set_label(render->spinner, "pausing", "pausing... (esc again to interrupt)");
        /* Before first content, cancel immediately; the loop maps the empty turn to a resumable
         * pause. */
        if (!render->stream_content_seen)
            return 1;
    }
    /* Retry countdown owns the label while the model is not streaming. */
    if (render->retry_deadline_at) {
        update_retry_label(render);
    } else if (render->md && md_in_table(render->md)) {
        /* Tick must surface a stalled table because no delta arrives to refresh its label. */
        if (!render->table_composing && render->last_text_at &&
            monotonic_ms() - render->last_text_at >= TABLE_IDLE_TIMEOUT_MS)
            render_table_spinner_show(render);
    }
    return interrupt_requested();
}

static int render_on_event(const struct stream_event *event, void *user)
{
    struct render_ctx *render = user;
    struct disp *disp = &render->disp;

    /* Stream completion closes the table-stall timing window. */
    if (event->kind == EV_DONE || event->kind == EV_ERROR)
        render->last_text_at = 0;
    /* The first post-retry event returns label control to normal rendering. */
    if (event->kind != EV_RETRY && render->retry_deadline_at) {
        render->retry_deadline_at = 0;
        spinner_set_label(render->spinner, "working", "working...");
    }

    /* Once output exists, a soft pause must preserve it by waiting for a turn seam. */
    if (event->kind == EV_TEXT_DELTA || event->kind == EV_REASONING_DELTA ||
        event->kind == EV_REASONING_ITEM || event->kind == EV_TOOL_CALL_START)
        render->stream_content_seen = 1;

    switch (event->kind) {
    case EV_TEXT_DELTA:
        render_text_delta(render, event->u.text_delta.text, strlen(event->u.text_delta.text));
        break;
    case EV_TOOL_CALL_START: {
        /* Tool arguments are invisible until dispatch. Supported providers stream calls
         * sequentially; a shared key prevents label flicker across a batch. */
        const char *tool_name = event->u.tool_call_start.name;
        render_stream_seam(render);
        if (tool_name && *tool_name) {
            char label[64];
            snprintf(label, sizeof(label), "[%s] composing...", tool_name);
            spinner_request_label(render->spinner, "compose", label);
        }
        break;
    }
    case EV_REASONING_ITEM:
        render_stream_seam(render);
        break;
    case EV_TOOL_CALL_END:
        /* Clear the composing label after args; settling hides normal dispatch latency. */
        spinner_request_label(render->spinner, "working", "working...");
        break;
    case EV_TOOL_CALL_DELTA:
        /* No live display: tool calls render as a single block during
         * dispatch so parallel calls don't visually interleave. */
        break;
    case EV_REASONING_DELTA: {
        /* Keep the thinking label even when reasoning text is hidden. */
        spinner_request_label(render->spinner, "thinking", "thinking...");
        const char *reasoning_text = event->u.reasoning_delta.text;
        if (!render->show_reasoning || !reasoning_text || !*reasoning_text)
            break;
        render_transition(render, RS_REASONING);
        render_text_chunk(render, reasoning_text, strlen(reasoning_text));
        break;
    }
    case EV_RETRY: {
        /* Store the next-attempt deadline so ticks can repaint the countdown immediately. */
        render->retry_deadline_at = monotonic_ms() + event->u.retry.delay_ms;
        render->retry_attempt = event->u.retry.attempt + 1;
        render->retry_max = event->u.retry.max_attempts;
        update_retry_label(render);
        break;
    }
    case EV_PROGRESS: {
        /* Report uncached prefill work so progress starts at zero despite cache reuse. A fully
         * cached prompt has no work to report. */
        long total_tokens = event->u.progress.total;
        long cached_tokens = event->u.progress.cache;
        long processed_tokens = event->u.progress.processed;
        long uncached_total = total_tokens - cached_tokens;
        if (uncached_total <= 0)
            break;
        long uncached_processed = processed_tokens - cached_tokens;
        if (uncached_processed < 0)
            uncached_processed = 0;
        int percentage = (int)((uncached_processed * 100) / uncached_total);
        if (percentage < 0)
            percentage = 0;
        if (percentage > 100)
            percentage = 100;
        char label[32];
        snprintf(label, sizeof(label), "processing... %d%%", percentage);
        /* One key preserves the settle clock while percentages refresh. */
        spinner_request_label(render->spinner, "processing", label);
        break;
    }
    case EV_DONE:
        disp_flush(disp);
        break;
    case EV_ERROR:
        /* Closing the render state flushes partial text before the error line. */
        render_open_block(render);
        disp_raw(disp, theme_open(THEME_ERROR));
        disp_printf(disp, "[error: %s]", event->u.error.message);
        disp_raw(disp, ANSI_RESET);
        disp_putc(disp, '\n');
        disp_flush(disp);
        break;
    }

    return 0;
}

/* Match interrupt_init()'s two-TTY gate; otherwise abnormal-exit restoration is not installed
 * and a hidden cursor could leak to the parent shell. */
static int cursor_supported(void)
{
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

static void cursor_show(void)
{
    if (!cursor_supported())
        return;
    fputs(ANSI_CURSOR_SHOW, stdout);
    fflush(stdout);
}

static void cursor_hide(void)
{
    if (!cursor_supported())
        return;
    fputs(ANSI_CURSOR_HIDE, stdout);
    fflush(stdout);
}

static const char *banner_bar(char *buffer, size_t size);
static void print_banner_identity(FILE *out, const struct provider *provider,
                                  const struct agent_session *session);

/* Default to `less -R` so the pager interprets ANSI color. */
static int view_pager_open(struct spawn_pipe *pipe)
{
    const char *pager = getenv("PAGER");
    if (!pager || !*pager)
        pager = "less -R";
    /* Keep pager signals in the child; early pager exit must become EPIPE, not terminate hax. */
    return spawn_pipe_open_write(pipe, pager);
}

static char *capture_paste(void *user)
{
    (void)user;
    return paste_image_capture();
}

/* Bracketed paste bypasses the Ctrl-V hook, so convert file URIs in the body filter too. */
static char *filter_paste(const char *text, void *user)
{
    (void)user;
    return paste_image_uris_to_paths(text);
}

/* View callbacks borrow agent_run's live state so vector growth and provider replacement cannot
 * stale captured pointers. */
static void show_transcript_cb(void *user)
{
    const struct agent_state *state = user;
    struct spawn_pipe pager;
    if (view_pager_open(&pager) < 0)
        return;
    /* Only the model-visible window: a compacted prefix is what the seed stands in for. */
    struct context window = agent_session_context(state->session);
    transcript_render(pager.stream, window.system_prompt, window.tools, window.n_tools,
                      window.items, window.n_items);
    spawn_pipe_close(&pager);
}

/* Render history through a private context so paging cannot disturb live display state. Resolve
 * cursor-addressed output before starting the pager. Provider switches are not history items, so
 * the banner identifies the current selection. */
static void show_history_cb(void *user)
{
    const struct agent_state *state = user;
    const struct agent_session *session = state->session;
    struct render_ctx render = {.show_reasoning = reasoning_visible()};
    char *output = NULL;
    size_t output_len = 0;
    FILE *memory_stream = open_memstream(&output, &output_len);
    if (!memory_stream)
        return;
    render.disp.out = memory_stream;
    if (markdown_enabled())
        render.md = md_new(md_emit_to_disp, &render.disp, md_cols());

    print_banner_identity(memory_stream, state->provider, session);
    /* Pager progress is byte-based, so include the prompt count when nonzero. */
    char bar[32];
    size_t prompts = agent_user_turn_count(session);
    if (prompts > 0)
        fprintf(memory_stream,
                "%s " ANSI_DIM "conversation history · %zu prompt%s" ANSI_BOLD_OFF "\n",
                banner_bar(bar, sizeof(bar)), prompts, prompts == 1 ? "" : "s");
    else
        fprintf(memory_stream, "%s " ANSI_DIM "conversation history" ANSI_BOLD_OFF "\n",
                banner_bar(bar, sizeof(bar)));
    /* Direct banner writes leave one committed newline outside disp's bookkeeping. */
    render.disp.trail = 1;

    if (session->n_items == 0) {
        render_open_block(&render);
        disp_raw(&render.disp, ANSI_DIM);
        disp_printf(&render.disp, "(nothing in this conversation yet)");
        disp_raw(&render.disp, ANSI_RESET);
        disp_putc(&render.disp, '\n');
    } else {
        history_render(&render, HISTORY_FULL, session->items, session->n_items, 0);
    }
    /* Flush the final Markdown tail before resolving terminal output. */
    render_transition(&render, RS_IDLE);
    disp_emit_held(&render.disp);
    md_free(render.md);
    fclose(memory_stream);

    struct spawn_pipe pager;
    if (view_pager_open(&pager) == 0) {
        vt_resolve(output, output_len, pager.stream);
        spawn_pipe_close(&pager);
    }
    free(output);
}

static const char *banner_bar(char *buffer, size_t size)
{
    snprintf(buffer, size, "%s▌%s", theme_open(THEME_CHROME), theme_close(THEME_CHROME));
    return buffer;
}

/* The identity row is shared with paged history; the REPL-only key tips are not. */
static void print_banner_identity(FILE *out, const struct provider *provider,
                                  const struct agent_session *session)
{
    char bar[32];
    banner_bar(bar, sizeof(bar));
    /* A preset may change the system prompt, so its stance must remain visible. SGR 22 exempts
     * the tinted token from the surrounding dim run; ANSI_DIM resumes it afterward. */
    const char *preset = config_str("preset");
    char *stance = (preset && *preset)
                       ? xasprintf(ANSI_BOLD_OFF "%s[%s]%s" ANSI_DIM " ", theme_open(THEME_STANCE),
                                   preset, theme_close(THEME_STANCE))
                       : xstrdup("");
    if (!provider) {
        fprintf(out,
                "%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM
                "%s› no provider — use /provider" ANSI_BOLD_OFF "\n",
                bar, stance);
        free(stance);
        return;
    }
    const char *provider_name = provider->name ? provider->name : "?";
    const char *model_label = session->model_label ? session->model_label : session->model;
    if (!session->model || !*session->model)
        fprintf(out,
                "%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM
                "%s› %s · no model — use /model" ANSI_BOLD_OFF "\n",
                bar, stance, provider_name);
    else if (session->effort)
        fprintf(out,
                "%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM "%s› %s · %s · %s" ANSI_BOLD_OFF
                "\n",
                bar, stance, provider_name, model_label, session->effort);
    else
        fprintf(out,
                "%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM "%s› %s · %s" ANSI_BOLD_OFF "\n",
                bar, stance, provider_name, model_label);
    free(stance);
}

void agent_print_banner(const struct provider *provider, const struct agent_session *session)
{
    print_banner_identity(stdout, provider, session);
    char bar[32];
    printf("%s " ANSI_DIM "ctrl-d quit · try /help" ANSI_BOLD_OFF "\n",
           banner_bar(bar, sizeof(bar)));
}

int agent_apply_settings(struct agent_state *state, struct provider *provider, int announce)
{
    struct agent_session *session = state->session;
    struct provider *previous_provider = state->provider;
    int provider_changed = provider != previous_provider;
    /* Snapshot the model before reconfigure overwrites it, to tell a real
     * /model change from a /provider or /effort apply that left it the same. */
    char *previous_model = session->model ? xstrdup(session->model) : NULL;
    if (agent_session_reconfigure(session, provider) != 0) {
        free(previous_model);
        return -1;
    }

    /* Refresh only after validation so a rolled-back selection never changes the display. */
    agent_display_refresh(state);

    /* Provider ownership transfers only after session reconfiguration succeeds. */
    if (provider_changed) {
        state->provider = provider;
        if (previous_provider)
            previous_provider->destroy(previous_provider);
    }

    /* Provider identity affects probed metadata even when the model id is unchanged. */
    int model_changed =
        (previous_model == NULL) != (session->model == NULL) ||
        (previous_model && session->model && strcmp(previous_model, session->model) != 0);
    free(previous_model);
    if (provider_changed || model_changed)
        model_meta_refresh(provider, session->model);

    /* Reconfiguration rebuilds the system prompt, including its model-specific environment. */
    transcript_log_reset(state->transcript, session->system_prompt, session->tools,
                         session->n_tools);
    transcript_log_append(state->transcript, session->items, session->n_items);

    /* Metadata is staged until the next append, so unused selections stay out of the log. */
    session_log_set_meta(state->session_log, agent_provider_log_name(provider), session->model,
                         session->model_label, session->effort, config_str("preset"));

    if (!announce)
        return 0;

    /* Replace a stale startup banner; mid-conversation a banner would imply a reset. */
    if (session->n_items == 0) {
        render_open_block(state->render);
        agent_print_banner(provider, session);
        state->render->disp.trail = 1; /* banner bypasses disp */
        fflush(stdout);
        return 0;
    }

    /* Selection notices are display-only; the model cannot act on them. */
    const char *model_label = session->model_label ? session->model_label : session->model;
    /* Include the stance because a preset can change more than provider and model. */
    const char *preset = config_str("preset");
    char *stance = (preset && *preset) ? xasprintf("[%s] ", preset) : xstrdup("");
    char *label = session->effort ? xasprintf("switched to %s%s · %s · %s", stance,
                                              provider->name ? provider->name : "?", model_label,
                                              session->effort)
                                  : xasprintf("switched to %s%s · %s", stance,
                                              provider->name ? provider->name : "?", model_label);
    free(stance);

    render_open_block(state->render);
    disp_raw(&state->render->disp, ANSI_DIM);
    disp_printf(&state->render->disp, "%s", label);
    disp_raw(&state->render->disp, ANSI_RESET);
    disp_putc(&state->render->disp, '\n');
    disp_flush(&state->render->disp);
    free(label);
    return 0;
}

/* Swapping or cutting history invalidates both resumable state and compaction debt tied to the
 * old tail. */
static void clear_resume_state(struct agent_state *state)
{
    state->resume_reason = AGENT_RESUME_NONE;
    state->resume_has_marker = 0;
    state->compaction_deferred = 0;
}

void agent_new_conversation(struct agent_state *state)
{
    clear_resume_state(state);
    agent_session_reset(state->session);
    transcript_log_reset(state->transcript, state->session->system_prompt, state->session->tools,
                         state->session->n_tools);
    session_log_reset(state->session_log);
    /* Old-turn temporary files are unreachable after history is reset. */
    tempfiles_cleanup();
    agent_spend_free(&state->stats.spend);
    memset(&state->stats, 0, sizeof(state->stats));
    agent_print_banner(state->provider, state->session);
}

/* Redraw with every prompt so slash-command output cannot hide the empty-send meaning. */
static void render_resume_hint(struct render_ctx *render, enum agent_resume_reason reason)
{
    const char *status;
    const char *action = "enter to continue";
    switch (reason) {
    case AGENT_RESUME_PAUSED:
        status = "paused";
        break;
    case AGENT_RESUME_MAX_TURNS:
        status = "max turns reached";
        break;
    case AGENT_RESUME_INTERRUPTED:
        status = "interrupted";
        break;
    case AGENT_RESUME_ERROR:
        status = "provider error";
        action = "enter to retry";
        break;
    default:
        return;
    }
    disp_raw(&render->disp, ANSI_DIM);
    disp_printf(&render->disp, "[%s — %s]", status, action);
    disp_raw(&render->disp, ANSI_RESET);
    disp_putc(&render->disp, '\n');
    disp_putc(&render->disp, '\n'); /* one blank line between hint and prompt */
    /* Commit newlines before the editor erases and repaints the prompt row. */
    disp_emit_held(&render->disp);
    disp_flush(&render->disp);
}

/* Only typed prompts and compaction seeds render as replay anchors; continuations and task
 * notes would replay as an empty turn heading. */
static int is_replay_anchor(const struct item *item)
{
    return item->kind == ITEM_USER_MESSAGE &&
           (item->origin == ITEM_ORIGIN_NONE || item->origin == ITEM_ORIGIN_COMPACT_SEED);
}

/* Replay only the final visible user turn so restored context does not displace live history.
 * Anchor on its user item rather than a turn boundary because one prompt may span several model
 * turns. Non-interactive runs render no replay. */
static void replay_user_turn(struct render_ctx *render, const struct agent_session *session,
                             const char *heading)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return;

    size_t anchor_index = 0;
    int anchor_found = 0;
    size_t earlier_count = 0;
    for (size_t i = session->n_items; i-- > 0;) {
        if (!is_replay_anchor(&session->items[i]))
            continue;
        if (!anchor_found) {
            anchor_index = i;
            anchor_found = 1;
        } else {
            earlier_count++;
        }
    }

    /* Direct banner/picker writes leave held stale; trail already records the newline count. */
    render->disp.held = 0;

    render_open_block(render);
    disp_raw(&render->disp, ANSI_DIM);
    if (earlier_count > 0)
        disp_printf(&render->disp, "── %s · %zu earlier message%s · ctrl-o for full history ──",
                    heading, earlier_count, earlier_count == 1 ? "" : "s");
    else
        disp_printf(&render->disp, "── %s · ctrl-o for full history ──", heading);
    disp_raw(&render->disp, ANSI_RESET);
    disp_putc(&render->disp, '\n');

    if (anchor_found)
        history_render(render, HISTORY_BRIEF, session->items, session->n_items, anchor_index);

    /* Markdown may leave its final row open; terminate it before returning to the prompt. */
    render_transition(render, RS_IDLE);
    if (render->disp.trail == 0 && render->disp.held == 0)
        disp_putc(&render->disp, '\n');
    disp_emit_held(&render->disp);
    disp_flush(&render->disp);
}

void agent_resume_session(struct agent_state *state, const char *path)
{
    /* Claim activity before reading; another process may run the daily sweep concurrently. */
    (void)session_touch(path);
    struct agent_session *session = state->session;
    struct item *loaded_items = NULL;
    size_t loaded_item_count = 0;
    struct session_meta metadata;
    if (session_load(path, &loaded_items, &loaded_item_count, &metadata) != 0 ||
        loaded_item_count == 0) {
        free(loaded_items);
        session_meta_free(&metadata);
        ui_error("could not read session");
        /* Replace the picker's stale trail count with the error line's committed newline. */
        state->render->disp.trail = 1;
        return;
    }

    /* Close the prior log before restoring settings so it cannot record the switch. */
    session_log_close(state->session_log);
    state->session_log = NULL;

    /* Restore settings before swapping history so announcements are framed against the
     * conversation being left and new logs use the restored system prompt and model. */
    select_restore_session(state, metadata.provider, metadata.model, metadata.effort,
                           metadata.preset);

    for (size_t i = 0; i < session->n_items; i++)
        item_free(&session->items[i]);
    free(session->items);
    session->items = loaded_items;
    session->n_items = loaded_item_count;
    session->cap_items = loaded_item_count;

    /* Re-evaluate recording after provider restoration; the starting provider may have disabled
     * it. Recorded metadata lets session_log_set_meta stage a failed restore as a switch. */
    if (agent_recording_enabled(state->provider))
        state->session_log =
            session_log_resume(path, metadata.provider, metadata.model, metadata.effort,
                               metadata.preset, loaded_item_count);
    /* Stage a failed selection restore without changing the file until a turn is appended. */
    session_log_set_meta(state->session_log, agent_provider_log_name(state->provider),
                         session->model, session->model_label, session->effort,
                         config_str("preset"));
    session_meta_free(&metadata);
    transcript_log_reset(state->transcript, session->system_prompt, session->tools,
                         session->n_tools);
    transcript_log_append(state->transcript, session->items, session->n_items);

    /* Loaded history may still reference tracked temporary files, including files shared by fork
     * prefixes. */
    clear_resume_state(state);
    replay_user_turn(state->render, session, "resumed");
}

/* This predicate must match session.c's JSONL rule: /undo combines its file offset with this
 * in-memory turn count. */
static int is_typed_prompt(const struct item *item)
{
    return item->kind == ITEM_USER_MESSAGE && item->origin == ITEM_ORIGIN_NONE;
}

static int turn_item_index(const struct agent_session *session, size_t turn_index,
                           size_t *item_index_out)
{
    size_t turns_seen = 0;
    for (size_t i = 0; i < session->n_items; i++) {
        if (is_typed_prompt(&session->items[i])) {
            if (turns_seen == turn_index) {
                *item_index_out = i;
                return 0;
            }
            turns_seen++;
        }
    }
    return -1;
}

size_t agent_user_turn_count(const struct agent_session *session)
{
    size_t count = 0;
    for (size_t i = 0; i < session->n_items; i++)
        if (is_typed_prompt(&session->items[i]))
            count++;
    return count;
}

const char *agent_user_turn_text(const struct agent_session *session, size_t turn_index)
{
    size_t item_index;
    if (turn_item_index(session, turn_index, &item_index) != 0)
        return NULL;
    return session->items[item_index].text;
}

/* Shared tail of /undo and /fork. `cut_index` is the item truncation point;
 * `turn_index` identifies the first discarded prompt to stage for recall. */
static void reshape_after_cut(struct agent_state *state, size_t cut_index, size_t turn_index,
                              const char *heading)
{
    struct agent_session *session = state->session;
    clear_resume_state(state);

    /* Capture recall text before freeing the discarded item. */
    const char *recall_text = agent_user_turn_text(session, turn_index);
    free(state->pending_recall);
    state->pending_recall = recall_text ? xstrdup(recall_text) : NULL;

    size_t old_item_count = session->n_items;
    for (size_t i = cut_index; i < session->n_items; i++)
        item_free(&session->items[i]);
    session->n_items = cut_index;

    /* A destructive cut invalidates the server-reported window snapshot; retained usage items do
     * not reliably describe the new tail. Cumulative totals remain valid. */
    if (cut_index < old_item_count) {
        state->stats.latest_context_tokens = 0;
        state->stats.context_limit = 0;
    }

    transcript_log_reset(state->transcript, session->system_prompt, session->tools,
                         session->n_tools);
    transcript_log_append(state->transcript, session->items, session->n_items);
    /* Cleanup is all-or-nothing, and the retained prefix may still reference tracked files. */
    replay_user_turn(state->render, session, heading);
}

void agent_undo(struct agent_state *state, size_t turn_index)
{
    struct agent_session *session = state->session;
    size_t cut_index;
    if (turn_item_index(session, turn_index, &cut_index) != 0)
        return;
    if (cut_index > 0 && session->items[cut_index - 1].kind == ITEM_TURN_BOUNDARY)
        cut_index--;

    size_t removed_turns = agent_user_turn_count(session) - turn_index;

    /* Truncate the on-disk record first; on I/O failure bail with history
     * intact. The file keeps the old branch and its high-water mark, so
     * truncating memory too would later append onto that stale branch. */
    if (session_log_truncate(state->session_log, turn_index, cut_index) != 0) {
        ui_error("could not truncate the session file; conversation left unchanged");
        state->render->disp.trail = 1;
        return;
    }

    char heading[64];
    snprintf(heading, sizeof(heading), "undid %zu turn%s", removed_turns,
             removed_turns == 1 ? "" : "s");
    reshape_after_cut(state, cut_index, turn_index, heading);
}

void agent_fork(struct agent_state *state, size_t turn_index)
{
    struct agent_session *session = state->session;
    size_t cut_index;
    if (turn_index >= agent_user_turn_count(session)) {
        cut_index = session->n_items;
    } else {
        if (turn_item_index(session, turn_index, &cut_index) != 0)
            return;
        if (cut_index > 0 && session->items[cut_index - 1].kind == ITEM_TURN_BOUNDARY)
            cut_index--;
    }

    /* Without a materialized log, fork cannot preserve the original branch. */
    if (!session_log_materialized(state->session_log)) {
        ui_error("/fork needs session recording (it is disabled or unavailable)");
        state->render->disp.trail = 1;
        return;
    }

    const char *source_path = session_log_path(state->session_log);
    char *new_path = NULL;
    if (session_fork_file(source_path, turn_index, &new_path) != 0) {
        ui_error("could not create fork");
        state->render->disp.trail = 1;
        return;
    }
    /* Open the new logger before closing the old one so failure is atomic. It starts with copied
     * metadata, then stages the live selection for the branch's first new turn. */
    struct session_meta fork_metadata;
    session_read_meta(new_path, &fork_metadata);
    struct session_log *new_session_log =
        session_log_resume(new_path, fork_metadata.provider, fork_metadata.model,
                           fork_metadata.effort, fork_metadata.preset, cut_index);
    session_meta_free(&fork_metadata);
    if (!new_session_log) {
        unlink(new_path);
        free(new_path);
        ui_error("could not open the fork session file; conversation left unchanged");
        state->render->disp.trail = 1;
        return;
    }
    session_log_set_meta(new_session_log, agent_provider_log_name(state->provider), session->model,
                         session->model_label, session->effort, config_str("preset"));
    free(new_path);
    session_log_close(state->session_log);
    state->session_log = new_session_log;

    reshape_after_cut(state, cut_index, turn_index, "forked");
}

/* Compaction usage contributes to session totals; compact_run owns history assembly. */
struct compact_event_ctx {
    struct session_stats *stats;
    struct render_ctx *render;
    const struct provider *provider;
    const char *model;
};

static int compact_on_event(const struct stream_event *event, void *user)
{
    struct compact_event_ctx *ctx = user;
    const struct stream_usage *usage = NULL;
    if (event->kind == EV_DONE)
        usage = &event->u.done.usage;
    else if (event->kind == EV_ERROR)
        usage = event->u.error.usage;
    if (!usage)
        return 0;

    stats_account_usage(ctx->stats, usage, ctx->provider, ctx->model);
    return 0;
}

/* Compaction has no pause seam, so either interrupt cancels the retriable transaction. */
static int compact_tick(void *user)
{
    struct compact_event_ctx *ctx = user;
    return agent_stream_tick(ctx->render) || interrupt_soft_requested();
}

static int compact_cancelled(void *user)
{
    (void)user;
    interrupt_settle();
    return interrupt_requested() || interrupt_soft_requested();
}

static void compact_notice(struct render_ctx *render, const char *format, ...)
{
    va_list args;
    char message[256];
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    render_open_block(render);
    disp_raw(&render->disp, ANSI_DIM);
    disp_printf(&render->disp, "── %s ──", message);
    disp_raw(&render->disp, ANSI_RESET);
    disp_putc(&render->disp, '\n');
    disp_flush(&render->disp);
}

int agent_compact(struct agent_state *state, const char *instructions, int automatic)
{
    struct agent_session *session = state->session;
    struct provider *provider = state->provider;
    struct render_ctx *render = state->render;

    if (!provider) {
        if (!automatic)
            compact_notice(render, "no provider selected — use /provider");
        return 0;
    }
    if (!session->model || !*session->model) {
        if (!automatic)
            compact_notice(render, "no model selected — use /model (or /provider)");
        return 0;
    }
    if (session->n_items == 0) {
        if (!automatic)
            compact_notice(render, "nothing to compact");
        return 0;
    }

    render_transition(render, RS_IDLE);

    /* compact_run owns the transaction; this layer supplies UI, cancellation, and accounting. */
    render_stream_begin(render);
    spinner_set_label(render->spinner, "compacting", "compacting...");
    struct compact_event_ctx event_ctx = {
        .stats = &state->stats,
        .render = render,
        .provider = provider,
        .model = session->model,
    };
    struct compact_params params = {
        .session = session,
        .provider = provider,
        .slog = state->session_log,
        .tlog = state->transcript,
        .instructions = instructions,
        .hooks =
            {
                .user = &event_ctx,
                .observe = compact_on_event,
                .tick = compact_tick,
                .cancelled = compact_cancelled,
            },
    };

    interrupt_clear();
    interrupt_arm();
    struct compact_result result;
    compact_run(&params, &result);
    /* Cancelled attempts emit no terminal event, so the transaction reports
     * the authoritative request count separately from usage observation. */
    state->stats.requests += result.attempts;
    interrupt_disarm();
    render_transition(render, RS_IDLE);

    int compacted = result.outcome == COMPACT_COMPLETE;
    /* The snapshot describes the window the seed replaced; retaining it could immediately
     * recompact the fresh seed. */
    if (compacted) {
        state->stats.latest_context_tokens = 0;
        state->stats.context_limit = 0;
        /* Any successful compaction — manual included — settles a deferred
         * end-of-turn pass: the oversized history it referred to is gone. */
        state->compaction_deferred = 0;
    }
    /* Manual compaction moves the model past the resumable tail, so its summary seed must not
     * receive a stale continuation marker. */
    if (compacted && !automatic)
        clear_resume_state(state);
    switch (result.outcome) {
    case COMPACT_COMPLETE:
        compact_notice(render, "conversation compacted");
        break;
    case COMPACT_CANCELLED:
        compact_notice(render, "compaction cancelled");
        break;
    case COMPACT_PROVIDER_ERROR:
        compact_notice(render, "compaction failed: %s",
                       result.error_message ? result.error_message : "stream failed");
        break;
    case COMPACT_NO_SUMMARY:
        compact_notice(render, "compaction produced no summary");
        break;
    case COMPACT_NO_PROVIDER:
        if (!automatic)
            compact_notice(render, "no provider selected — use /provider");
        break;
    case COMPACT_NO_MODEL:
        if (!automatic)
            compact_notice(render, "no model selected — use /model (or /provider)");
        break;
    case COMPACT_EMPTY:
        if (!automatic)
            compact_notice(render, "nothing to compact");
        break;
    }
    compact_result_destroy(&result);
    return compacted;
}

struct repl_loop_ctx {
    struct agent_state *state;
};

static int repl_loop_on_event(const struct stream_event *event, void *user)
{
    struct repl_loop_ctx *ctx = user;
    return render_on_event(event, ctx->state->render);
}

static int repl_loop_tick(void *user)
{
    struct repl_loop_ctx *ctx = user;
    return agent_stream_tick(ctx->state->render);
}

static void repl_loop_turn_begin(void *user)
{
    struct repl_loop_ctx *ctx = user;
    struct render_ctx *render = ctx->state->render;
    render_stream_begin(render);
    if (render->md)
        md_reset(render->md, md_cols());
    render->disp.saw_text = 0;
    render->stream_content_seen = 0;
}

static void repl_loop_turn_end(const struct agent_loop_turn *loop_turn, void *user)
{
    struct repl_loop_ctx *ctx = user;
    struct agent_state *state = ctx->state;
    struct agent_session *session = state->session;
    const struct provider *provider = state->provider;
    struct session_stats *stats = &state->stats;
    const struct stream_usage *usage = &loop_turn->usage;

    stats->requests++;
    if (usage->input_tokens >= 0 && usage->output_tokens >= 0) {
        stats->latest_context_tokens = usage->input_tokens + usage->output_tokens;
        stats->context_limit = model_meta_context(provider, session->model);
    }
    stats_account_usage(stats, usage, provider, session->model);
}

static int repl_loop_checkpoint(void *user)
{
    (void)user;
    interrupt_settle();
    if (interrupt_requested())
        return AGENT_LOOP_SIG_ABORT;
    if (interrupt_soft_requested())
        return AGENT_LOOP_SIG_PAUSE;
    return AGENT_LOOP_SIG_NONE;
}

static void repl_loop_tool_seen(const struct item *call, void *user)
{
    struct repl_loop_ctx *ctx = user;
    stats_count_tool_call(&ctx->state->stats, call->tool_name);
}

static struct item repl_loop_tool_call(const struct item *call, enum agent_loop_tool_action action,
                                       int image_input, void *user)
{
    struct repl_loop_ctx *ctx = user;
    struct render_ctx *render = ctx->state->render;
    if (action == AGENT_LOOP_TOOL_REFUSE) {
        render_transition(render, RS_IDLE);
        return dispatch_tool_refused(render, call);
    }
    if (action == AGENT_LOOP_TOOL_SKIP) {
        render_transition(render, RS_IDLE);
        return dispatch_tool_skipped(render, call);
    }
    return dispatch_tool_call(render, call, image_input);
}

static void repl_loop_compact(void *user)
{
    struct repl_loop_ctx *ctx = user;
    agent_compact(ctx->state, NULL, 1);
    /* Preserve cancellation for the loop checkpoint; otherwise re-arm after compaction. */
    if (!interrupt_requested() && !interrupt_soft_requested()) {
        interrupt_clear();
        interrupt_arm();
    }
}

static void repl_loop_task_note(const char *text, void *user)
{
    struct repl_loop_ctx *ctx = user;
    struct render_ctx *render = ctx->state->render;
    spinner_hide(render->spinner);
    render_transition(render, RS_IDLE);
    render_open_block(render);
    disp_raw(&render->disp, ANSI_DIM);
    disp_write(&render->disp, text, strlen(text));
    disp_raw(&render->disp, ANSI_RESET);
    disp_putc(&render->disp, '\n');
    disp_flush(&render->disp);
}

static int handle_slash_input(struct input *input, struct agent_state *state, const char *line)
{
    if (!*line)
        return 0;

    if (slash_dispatch(line, state) == SLASH_NOT_A_COMMAND)
        return 0;

    input_history_add_session(input, line);
    if (state->pending_recall) {
        input_history_add_session(input, state->pending_recall);
        free(state->pending_recall);
        state->pending_recall = NULL;
    }
    if (state->pending_preseed) {
        input_set_preseed(input, state->pending_preseed);
        free(state->pending_preseed);
        state->pending_preseed = NULL;
    }
    return 1;
}

static void set_resume_state(struct agent_state *state, const struct agent_loop_result *result)
{
    switch (result->outcome) {
    case AGENT_LOOP_COMPLETE:
        clear_resume_state(state);
        return;
    case AGENT_LOOP_PAUSED:
        state->resume_reason = AGENT_RESUME_PAUSED;
        state->resume_has_marker = 0;
        return;
    case AGENT_LOOP_MAX_TURNS:
        state->resume_reason = AGENT_RESUME_MAX_TURNS;
        state->resume_has_marker = 0;
        return;
    case AGENT_LOOP_INTERRUPTED:
        state->resume_reason = AGENT_RESUME_INTERRUPTED;
        break;
    case AGENT_LOOP_PROVIDER_ERROR:
        state->resume_reason = AGENT_RESUME_ERROR;
        break;
    }
    state->resume_has_marker = result->abort_marker_placed;
}

int agent_run(struct provider **provider_io, const struct hax_opts *options)
{
    /* Slash handlers replace state.provider; resynchronize this local after each dispatch and
     * return the final live provider to the caller. */
    struct provider *current_provider = *provider_io;
    struct agent_session session;
    agent_session_init(&session, current_provider, options);

    /* Recording controls new session and prompt-history writes, not reads. Mid-run resume
     * re-evaluates it after restoring the provider. */
    int recording_enabled = agent_recording_enabled(current_provider);

    /* Load resumed history before initializing dependent views and logs. An unreadable file is
     * fatal because starting fresh would use the wrong context. */
    size_t resumed_item_count = 0;
    /* Preserve recorded metadata until the resumed log is opened; flags may have overridden the
     * live selection. */
    struct session_meta resume_metadata;
    memset(&resume_metadata, 0, sizeof(resume_metadata));
    if (options->resume_path) {
        struct item *loaded_items = NULL;
        size_t loaded_item_count = 0;
        if (session_load(options->resume_path, &loaded_items, &loaded_item_count,
                         &resume_metadata) != 0) {
            hax_err("could not resume session '%s'", options->resume_path);
            session_meta_free(&resume_metadata);
            agent_session_free(&session);
            return 1;
        }
        session.items = loaded_items;
        session.n_items = loaded_item_count;
        session.cap_items = loaded_item_count;
        resumed_item_count = loaded_item_count;
    }

    putchar('\n');
    agent_print_banner(current_provider, &session);
    /* The embedded disp outlives its Markdown callback; spinner and Markdown handles are owned
     * by this frame. */
    struct render_ctx render = {.disp = {.out = stdout, .trail = 1},
                                .show_reasoning = reasoning_visible()};
    render.spinner = spinner_new("working...");
    render.md = markdown_enabled() ? md_new(md_emit_to_disp, &render.disp, md_cols()) : NULL;
    /* Replay needs the live renderer initialized first. */
    if (resumed_item_count > 0)
        replay_user_turn(&render, &session, "resumed");
    struct input *input = input_new();
    /* Prompt recall remains readable when recording is disabled. */
    input_history_open_default(input, recording_enabled);
    input_set_modal_completer(input, &file_mention_completer);
    input_set_paste_hook(input, capture_paste, NULL);
    input_set_paste_filter(input, filter_paste, NULL);
    /* Transcript logging is optional; its API is NULL-safe. */
    struct transcript_log *transcript =
        transcript_log_open(session.system_prompt, session.tools, session.n_tools);
    /* Slash handlers borrow this frame. Keep the session log on state because /resume can replace
     * and close it mid-run. */
    struct agent_state state = {.session = &session,
                                .provider = current_provider,
                                .transcript = transcript,
                                .render = &render};
    /* Raw mode clears IEXTEN, so Ctrl-O does not trigger BSD/macOS VDISCARD. */
    input_bind_modal_key(input, INPUT_KEY_CTRL('O'), show_history_cb, &state);
    input_bind_modal_key(input, INPUT_KEY_CTRL('T'), show_transcript_cb, &state);
    /* Resume continues the existing log without rewriting restored items; the API is NULL-safe
     * when recording is disabled. */
    if (recording_enabled)
        state.session_log =
            options->resume_path
                ? session_log_resume(options->resume_path, resume_metadata.provider,
                                     resume_metadata.model, resume_metadata.effort,
                                     resume_metadata.preset, resumed_item_count)
                : session_log_open(agent_provider_log_name(current_provider), session.model,
                                   session.model_label, session.effort, config_str("preset"));
    /* Stage flag-overridden selection metadata; it reaches disk only with the next turn. */
    if (options->resume_path)
        session_log_set_meta(state.session_log, agent_provider_log_name(current_provider),
                             session.model, session.model_label, session.effort,
                             config_str("preset"));
    session_meta_free(&resume_metadata);
    if (resumed_item_count > 0)
        transcript_log_append(transcript, session.items, session.n_items);
    /* Capture terminal state before raw input; non-TTY initialization is a no-op. */
    interrupt_init();
    interrupt_set_fatal_hook(bash_shell_pgids_kill);

    char prompt_buffer[64];

    for (;;) {
        disp_block_separator(&render.disp);
        /* Redraw the resumable hint so slash output cannot hide the empty-send meaning. */
        if (state.resume_reason != AGENT_RESUME_NONE)
            render_resume_hint(&render, state.resume_reason);
        /* Only a resumable turn gives an empty send a meaning; otherwise
         * the editor keeps swallowing bare Enter. */
        input_set_empty_submit(input, state.resume_reason != AGENT_RESUME_NONE);
        cursor_show();
        /* Rebuilt each iteration so a runtime theme change (/config theme …)
         * recolors the prompt instead of keeping the startup theme's bytes. */
        char *line = input_readline(input, build_prompt(prompt_buffer, sizeof(prompt_buffer)));
        cursor_hide();
        if (!line) {
            putchar('\n');
            break;
        }
        /* Empty input continues only resumable turns. */
        if (!*line && state.resume_reason == AGENT_RESUME_NONE) {
            free(line);
            continue;
        }

        /* Slash handlers may override this when they drive the display themselves. */
        render.disp.trail = 1;
        if (handle_slash_input(input, &state, line)) {
            current_provider = state.provider;
            free(line);
            continue;
        }
        if (*line)
            input_history_add(input, line);

        /* Provider absence takes precedence over a possibly configured model. */
        if (!current_provider) {
            /* ui_note bypasses disp, so restore its one-newline trail state. */
            disp_block_separator(&render.disp);
            ui_note("no provider selected — use /provider to choose one, then resend");
            render.disp.trail = 1;
            free(line);
            continue;
        }

        /* Keep unresolved-model prompts recallable while directing the user to a selector. */
        if (!session.model || !*session.model) {
            disp_block_separator(&render.disp);
            ui_note("no model selected — use /model (or /provider) to choose one, then resend");
            render.disp.trail = 1;
            free(line);
            continue;
        }

        /* Settle deferred compaction before appending steering input or sending another oversized
         * request. */
        int compacted = 0;
        if (state.compaction_deferred) {
            compacted = agent_compact(&state, NULL, 1);
            /* A newly latched interrupt belongs to compaction and cancels the whole send; retain
             * the debt for the next attempt. */
            if (interrupt_requested() || interrupt_soft_requested()) {
                free(line);
                continue;
            }
            /* Settled on success (agent_compact clears the flag), attempted
             * on failure: either way, don't retry ahead of every send. */
            state.compaction_deferred = 0;
        }

        /* After compaction, empty input continues from the summary seed without a stale marker. */
        int continued = 0;
        int typed_prompt = *line != 0;
        /* Boundaries precede fresh prompts. Marked resumes add a continuation item; clean resumes
         * let the loop append its boundary. */
        if (*line)
            agent_session_add_user(&session, line);
        else if (!compacted && state.resume_has_marker)
            agent_session_add_continuation(&session);
        else
            continued = 1;
        free(line);
        /* input_readline left the cursor at column 0 of a fresh row. */
        render.disp.trail = 1;

        /* A background metadata probe may refine effort before the first request; announce the
         * value that will actually be sent. */
        char *previous_effort = NULL;
        if (agent_session_resync_effort(&session, current_provider, &previous_effort)) {
            disp_block_separator(&render.disp);
            ui_note("effort %s → %s · %s", previous_effort ? previous_effort : "(unset)",
                    session.effort ? session.effort : "(unset)",
                    session.model_label ? session.model_label : "?");
            render.disp.trail = 1;
        }
        free(previous_effort);
        /* Stage the final pre-request selection; an already-written header is unaffected. */
        session_log_set_meta(state.session_log, agent_provider_log_name(current_provider),
                             session.model, session.model_label, session.effort,
                             config_str("preset"));
        /* Persist the prompt before entering a provider call that may hang or be interrupted. */
        agent_loop_flush_logs(transcript, state.session_log, session.items, session.n_items);

        /* Start the one-per-run catalog refresh while the model generates; warn only when stale
         * data may distort estimates. */
        if (current_provider->catalog_id) {
            long stale_days = catalog_prefetch();
            if (stale_days > 0) {
                disp_block_separator(&render.disp);
                ui_note("model catalog last refreshed %ld days ago — cost estimates may be stale",
                        stale_days);
                render.disp.trail = 1;
            }
        }

        /* Each request subsumes the prior prefix, so the latest reported usage is the current
         * window state. -1 means no request reported it. */
        long user_turn_context_tokens = -1;
        long user_turn_start_ms = monotonic_ms();
        int user_turn_errored = 0;

        /* Reset promoted spinner state before timing the new user turn. */
        spinner_set_label(render.spinner, "working", "working...");
        spinner_set_timer(render.spinner, user_turn_start_ms);

        /* Clear stale editor interrupts before arming first-Esc pause and second-Esc abort. */
        interrupt_clear();
        interrupt_arm();
        /* A positive max_turns pauses at a clean seam; zero means unlimited. */
        int max_turns = config_int("max_turns");
        struct repl_loop_ctx loop_ctx = {.state = &state};
        struct agent_loop_params loop_params = {
            .session = &session,
            .provider = current_provider,
            .tlog = state.transcript,
            .slog = state.session_log,
            .max_turns = max_turns > 0 ? max_turns : -1,
            .continued = continued,
            .hooks =
                {
                    .user = &loop_ctx,
                    .observe = repl_loop_on_event,
                    .tick = repl_loop_tick,
                    .turn_begin = repl_loop_turn_begin,
                    .turn_end = repl_loop_turn_end,
                    .checkpoint = repl_loop_checkpoint,
                    .tool_seen = repl_loop_tool_seen,
                    .tool_call = repl_loop_tool_call,
                    .compact = repl_loop_compact,
                    .task_note = repl_loop_task_note,
                },
        };
        struct agent_loop_result loop_result;
        agent_loop_run(&loop_params, &loop_result);
        user_turn_context_tokens = loop_result.last_context_tokens;
        user_turn_errored = loop_result.outcome == AGENT_LOOP_PROVIDER_ERROR;
        int user_turn_complete = loop_result.outcome == AGENT_LOOP_COMPLETE;
        set_resume_state(&state, &loop_result);
        agent_loop_result_destroy(&loop_result);
        interrupt_disarm();
        /* Snapshot Esc before auto-compaction clears the interrupt latch. */
        int user_pressed_escape = interrupt_soft_requested();
        spinner_set_timer(render.spinner, 0);

        /* Close active rendering before post-turn output can emit terminal control sequences. */
        render_transition(&render, RS_IDLE);

        /* History and the resume hint already expose interruptions; another live marker duplicates
         * them. */
        if (user_turn_complete && user_pressed_escape) {
            /* Confirm an Esc that arrived after the last pause point. */
            render_open_block(&render);
            disp_raw(&render.disp, ANSI_DIM);
            disp_printf(&render.disp, "[finished before pause]");
            disp_raw(&render.disp, ANSI_RESET);
            disp_putc(&render.disp, '\n');
            disp_flush(&render.disp);
        }

        /* Time worked counts errored/interrupted turns too — the wall time
         * was spent either way, and /session's total should reflect it. */
        long user_turn_ms = monotonic_ms() - user_turn_start_ms;
        state.stats.worked_ms += user_turn_ms;
        if (typed_prompt)
            state.stats.user_turns++;

        if (!user_turn_errored)
            display_stats_line(&render, current_provider, session.model, user_turn_context_tokens,
                               user_turn_ms, &state.stats);

        /* Auto-compact only completed, Esc-free turns. Clean pauses defer compaction until the
         * next send; errors and hard interrupts retain partial history for retry or explicit
         * compaction. */
        if (compact_should_auto(user_turn_context_tokens,
                                model_meta_context(current_provider, session.model))) {
            if (user_turn_complete && !user_pressed_escape)
                agent_compact(&state, NULL, 1);
            else if (user_turn_complete || state.resume_reason == AGENT_RESUME_PAUSED ||
                     state.resume_reason == AGENT_RESUME_MAX_TURNS)
                state.compaction_deferred = 1;
        }

        /* Esc means the user is already present; otherwise notify when the REPL becomes idle,
         * including errors and max-turn pauses. */
        if (!user_pressed_escape)
            notify_attention();
    }

    size_t running_tasks = task_running_count();
    if (running_tasks > 0)
        ui_note("stopping %zu running background task%s", running_tasks,
                running_tasks == 1 ? "" : "s");
    /* Resolve every uncollected task in the record — final status for finished ones,
     * killed-at-exit for the rest — so a resumed session never dangles on a
     * "[detached as task ...]" report or advertises output this exit destroys. */
    char *exit_note = task_exit_note();
    if (exit_note) {
        agent_session_append(&session, (struct item){
                                           .kind = ITEM_USER_MESSAGE,
                                           .text = exit_note,
                                           .origin = ITEM_ORIGIN_TASK_NOTE,
                                       });
        agent_loop_flush_logs(transcript, state.session_log, session.items, session.n_items);
    }
    task_registry_shutdown();

    const char *resume_hint = session_log_resume_hint(state.session_log);
    if (resume_hint)
        ui_note("resume with: hax --resume=%s", resume_hint);

    *provider_io = current_provider;

    spinner_free(render.spinner);
    input_free(input);
    if (render.md)
        md_free(render.md);
    transcript_log_close(transcript);
    session_log_close(state.session_log);
    agent_spend_free(&state.stats.spend);
    free(state.pending_preseed);
    agent_session_free(&session);
    return 0;
}
