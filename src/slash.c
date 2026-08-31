/* SPDX-License-Identifier: MIT */
#include "slash.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "agent_core.h"
#include "agent_usage.h"
#include "catalog.h"
#include "config.h"
#include "file_mention.h"
#include "login.h"
#include "model_meta.h"
#include "provider.h"
#include "select.h"
#include "session.h"
#include "session_picker.h"
#include "xalloc.h"
#include "render/disp.h"
#include "render/render_ctx.h"
#include "terminal/ansi.h"
#include "terminal/clipboard.h"
#include "terminal/picker.h"
#include "terminal/theme.h"
#include "terminal/ui.h"
#include "terminal/width.h"
#include "text/fmt.h"
#include "text/width.h"
#include "tools/task_registry.h"

/* Managed handlers leave disp bookkeeping accurate; raw handlers end on an untracked newline. */
enum command_display {
    COMMAND_DISPLAY_RAW,
    COMMAND_DISPLAY_MANAGED,
};

struct command_call {
    struct agent_state *state;
    const char *argument;
};

struct slash_command {
    const char *name;
    const char *alias;
    const char *summary;
    int accepts_argument;
    enum command_display display;
    void (*handler)(const struct command_call *call);
};

struct shortcut {
    const char *key;
    const char *description;
    int (*available)(void);
    const char *unavailable_note;
};

struct parsed_command {
    char *name;
    const char *argument;
};

static void run_new(const struct command_call *call);
static void run_resume(const struct command_call *call);
static void run_undo(const struct command_call *call);
static void run_fork(const struct command_call *call);
static void run_provider(const struct command_call *call);
static void run_model(const struct command_call *call);
static void run_effort(const struct command_call *call);
static void run_preset(const struct command_call *call);
static void run_preset_save(const struct command_call *call);
static void run_config(const struct command_call *call);
static void run_compact(const struct command_call *call);
static void run_copy(const struct command_call *call);
static void run_session(const struct command_call *call);
static void run_tasks(const struct command_call *call);
static void run_usage(const struct command_call *call);
static void run_login(const struct command_call *call);
static void run_logout(const struct command_call *call);
static void run_help(const struct command_call *call);

/* Registry order is also /help order. */
static const struct slash_command COMMANDS[] = {
    {
        .name = "new",
        .alias = "clear",
        .summary = "start a fresh conversation (optional: preset)",
        .accepts_argument = 1,
        .handler = run_new,
    },
    {
        .name = "resume",
        .summary = "resume a past conversation",
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_resume,
    },
    {
        .name = "undo",
        .summary = "revert conversation to before an earlier message (optional: turns back)",
        .accepts_argument = 1,
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_undo,
    },
    {
        .name = "fork",
        .summary = "branch a new session before an earlier message (optional: turns back)",
        .accepts_argument = 1,
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_fork,
    },
    {
        .name = "provider",
        .summary = "switch provider, then model and effort",
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_provider,
    },
    {
        .name = "model",
        .summary = "switch model, then effort",
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_model,
    },
    {
        .name = "effort",
        .summary = "set reasoning effort",
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_effort,
    },
    {
        .name = "preset",
        .summary = "switch to a config-defined preset (optional: name)",
        .accepts_argument = 1,
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_preset,
    },
    /* `/preset save` would conflict with a preset named "save". */
    {
        .name = "preset-save",
        .summary = "save the current selection as a preset (name, optional tint)",
        .accepts_argument = 1,
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_preset_save,
    },
    {
        .name = "config",
        .summary = "view or change settings (optional: key value)",
        .accepts_argument = 1,
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_config,
    },
    {
        .name = "compact",
        .summary = "summarize history to free up context (optional: focus instructions)",
        .accepts_argument = 1,
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_compact,
    },
    {
        .name = "copy",
        .summary = "copy last response to clipboard",
        .handler = run_copy,
    },
    {
        .name = "tasks",
        .summary = "list background tasks (optional: kill <id>... | kill all)",
        .accepts_argument = 1,
        .handler = run_tasks,
    },
    {
        .name = "session",
        .summary = "show this session's info and usage totals",
        .handler = run_session,
    },
    {
        .name = "usage",
        .summary = "show provider account usage",
        .handler = run_usage,
    },
    {
        .name = "login",
        .summary = "log in to a provider account, managed by hax (optional: provider)",
        .accepts_argument = 1,
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_login,
    },
    {
        .name = "logout",
        .summary = "log out and remove a hax-managed login (optional: provider)",
        .accepts_argument = 1,
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_logout,
    },
    {
        .name = "help",
        .summary = "show this help",
        .handler = run_help,
    },
};
#define N_COMMANDS (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

/* hax-specific or non-obvious bindings only. The full readline-style
 * motion set (Ctrl-A/E/B/F/W/U/K/H, arrows, Home/End) is intentionally
 * omitted: users who know readline already know them, and listing
 * everything would push the more useful bindings off the screen. */
static const struct shortcut SHORTCUTS[] = {
    {.key = "enter", .description = "submit prompt"},
    {.key = "shift-enter",
     .description = "insert newline (terminal must be configured to send LF)"},
    {.key = "esc", .description = "pause after the current step to steer the model"},
    {.key = "esc esc", .description = "interrupt model or running tool immediately"},
    {.key = "ctrl-c", .description = "cancel current prompt line"},
    {.key = "ctrl-d", .description = "quit (on empty prompt)"},
    {.key = "ctrl-l", .description = "clear screen and redraw prompt"},
    {.key = "ctrl-g", .description = "edit prompt in $EDITOR"},
    {.key = "ctrl-o", .description = "view conversation history in $PAGER"},
    {.key = "ctrl-t", .description = "view model-facing transcript in $PAGER"},
    {.key = "ctrl-v", .description = "paste image (or text) from clipboard"},
    {.key = "@ + tab",
     .description = "pick a project file to mention",
     .available = file_mention_available,
     .unavailable_note = "(fzf not installed)"},
};
#define N_SHORTCUTS (sizeof(SHORTCUTS) / sizeof(SHORTCUTS[0]))

static int parse_command(const char *line, struct parsed_command *parsed)
{
    if (!line || line[0] != '/')
        return 0;

    const char *name = line + 1;
    const char *cursor = name;
    while (*cursor && !isspace((unsigned char)*cursor)) {
        unsigned char c = (unsigned char)*cursor;
        /* Restrict command-shaped input so paths and terminal control bytes pass through. */
        if (!isalnum(c) && c != '_' && c != '-')
            return 0;
        cursor++;
    }
    if (cursor == name)
        return 0;

    size_t name_length = (size_t)(cursor - name);
    parsed->name = xmalloc(name_length + 1);
    memcpy(parsed->name, name, name_length);
    parsed->name[name_length] = '\0';

    while (*cursor && isspace((unsigned char)*cursor))
        cursor++;
    parsed->argument = *cursor ? cursor : NULL;
    return 1;
}

static const struct slash_command *find_command(const char *name)
{
    for (size_t i = 0; i < N_COMMANDS; i++) {
        if (strcmp(COMMANDS[i].name, name) == 0 ||
            (COMMANDS[i].alias && strcmp(COMMANDS[i].alias, name) == 0))
            return &COMMANDS[i];
    }
    return NULL;
}

enum slash_result slash_dispatch(const char *line, struct agent_state *state)
{
    struct parsed_command parsed;
    if (!parse_command(line, &parsed))
        return SLASH_NOT_A_COMMAND;

    struct disp *disp = &state->render->disp;
    disp_block_separator(disp);

    enum slash_result result;
    const struct slash_command *command = find_command(parsed.name);
    if (!command) {
        ui_error("unknown command: /%s. type /help for the list.", parsed.name);
        result = SLASH_UNKNOWN;
        goto raw_output;
    }
    if (parsed.argument && !command->accepts_argument) {
        ui_error("/%s takes no arguments.", parsed.name);
        result = SLASH_BAD_USAGE;
        goto raw_output;
    }

    struct command_call call = {
        .state = state,
        .argument = command->accepts_argument ? parsed.argument : NULL,
    };
    command->handler(&call);
    if (command->display == COMMAND_DISPLAY_RAW)
        disp_sync_external_line(disp);
    free(parsed.name);
    return SLASH_HANDLED;

raw_output:
    disp_sync_external_line(disp);
    free(parsed.name);
    return result;
}

/* ---------- /new ---------- */

static void run_new(const struct command_call *call)
{
    /* Apply first so an invalid preset cannot discard the current conversation. */
    if (call->argument && select_preset(call->state, call->argument, 0) != 0)
        return;
    agent_new_conversation(call->state);
}

/* ---------- /resume ---------- */

static void run_resume(const struct command_call *call)
{
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) {
        ui_error("cannot determine working directory");
        return;
    }
    const char *current_path = session_log_path(call->state->session_log);
    int picker_opened = 0;
    char *path = session_picker_run(cwd, current_path, &picker_opened);
    /* An opened picker erases back to the separator row. Without one, a raw note ends one row
     * below it and the display state must follow. */
    if (!picker_opened)
        disp_sync_external_line(&call->state->render->disp);
    if (!path)
        return;
    agent_resume_session(call->state, path);
    free(path);
}

/* ---------- /undo, /fork ---------- */

/* The picker clips labels to its row width; extra cells remain searchable. */
#define TURN_LABEL_CELLS 512

enum history_action {
    HISTORY_UNDO,
    HISTORY_FORK,
};

static long choose_history_turn(struct agent_session *session, size_t turn_count,
                                enum history_action action)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return -1;

    struct picker_item *items = xcalloc(turn_count, sizeof(*items));
    char **labels = xmalloc(turn_count * sizeof(*labels));
    for (size_t turn_index = 0; turn_index < turn_count; turn_index++) {
        const char *text = agent_user_turn_text(session, turn_index);
        char *flat = flatten_for_display(text ? text : "");
        labels[turn_index] = truncate_for_display(flat, TURN_LABEL_CELLS);
        free(flat);
        items[turn_index].label =
            (labels[turn_index] && labels[turn_index][0]) ? labels[turn_index] : "(empty)";
    }

    struct picker_opts options = {
        .title =
            action == HISTORY_UNDO ? "revert to before which message" : "fork before which message",
        .items = items,
        .item_count = turn_count,
        .initial_index = turn_count - 1,
        .repeat_clipped_label = 1,
    };
    long selected_index = picker_run(&options);

    for (size_t turn_index = 0; turn_index < turn_count; turn_index++)
        free(labels[turn_index]);
    free(labels);
    free(items);
    return selected_index;
}

static void run_history_action(const struct command_call *call, enum history_action action)
{
    struct agent_state *state = call->state;
    struct agent_session *session = state->session;
    const char *verb = action == HISTORY_UNDO ? "undo" : "fork";
    size_t turn_count = agent_user_turn_count(session);

    long turn_index;
    if (call->argument) {
        /* N counts turns back from the end: 1 is the most recent turn, `turn_count`
         * the first. /fork also accepts 0 — the current tip — which clones the
         * whole conversation; that stays valid even when the only user item is
         * a compaction seed (turn_count 0), as long as there's history to copy.
         * "undo nothing" is meaningless, so /undo starts at 1. */
        long minimum_turns_back = action == HISTORY_UNDO ? 1 : 0;
        char *end;
        long turns_back = strtol(call->argument, &end, 10);
        while (isspace((unsigned char)*end))
            end++;
        if (action == HISTORY_FORK && *end == '\0' && turns_back == 0) {
            if (session->n_items == 0) {
                ui_note("nothing to fork yet");
                disp_sync_external_line(&state->render->disp);
                return;
            }
            agent_fork(state, turn_count);
            return;
        }
        if (*end != '\0' || turns_back < minimum_turns_back || (size_t)turns_back > turn_count) {
            if (turn_count == 0)
                ui_note("nothing to %s yet", verb);
            else
                ui_error("/%s takes a number of turns between %ld and %zu", verb,
                         minimum_turns_back, turn_count);
            disp_sync_external_line(&state->render->disp);
            return;
        }
        turn_index = (long)turn_count - turns_back;
    } else {
        if (turn_count == 0) {
            ui_note("nothing to %s yet", verb);
            disp_sync_external_line(&state->render->disp);
            return;
        }
        turn_index = choose_history_turn(session, turn_count, action);
        if (turn_index < 0) {
            /* A shown picker erased back to its start row, so leave disp as
             * the dispatcher's separator set it. With no tty there was no
             * picker and no way to choose — point at the argument form. */
            if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
                ui_note("/%s needs a number of turns when not interactive", verb);
                disp_sync_external_line(&state->render->disp);
            }
            return;
        }
    }

    if (action == HISTORY_UNDO)
        agent_undo(state, (size_t)turn_index);
    else
        agent_fork(state, (size_t)turn_index);
}

static void run_undo(const struct command_call *call)
{
    run_history_action(call, HISTORY_UNDO);
}

static void run_fork(const struct command_call *call)
{
    run_history_action(call, HISTORY_FORK);
}

/* ---------- forwarding handlers ---------- */

static void run_provider(const struct command_call *call)
{
    select_provider(call->state);
}

static void run_model(const struct command_call *call)
{
    select_model(call->state);
}

static void run_effort(const struct command_call *call)
{
    select_effort(call->state);
}

static void run_preset(const struct command_call *call)
{
    select_preset(call->state, call->argument, 1);
}

static void run_preset_save(const struct command_call *call)
{
    select_preset_save(call->state, call->argument);
}

static void run_config(const struct command_call *call)
{
    select_config(call->state, call->argument);
}

static void run_compact(const struct command_call *call)
{
    agent_compact(call->state, call->argument, 0);
}

/* ---------- /copy ---------- */

static const char *last_response_text(const struct agent_session *session)
{
    if (!session)
        return NULL;
    for (size_t i = session->n_items; i > 0; i--) {
        const struct item *item = &session->items[i - 1];
        if (item->kind == ITEM_ASSISTANT_MESSAGE && item->text && item->text[0])
            return item->text;
    }
    return NULL;
}

static void run_copy(const struct command_call *call)
{
    const char *text = last_response_text(call->state->session);
    if (!text) {
        ui_note("no assistant response to copy");
        return;
    }
    size_t byte_count = strlen(text);
    const char *error = NULL;
    if (clipboard_copy(text, byte_count, &error) == 0) {
        ui_note("copied %zu byte%s to clipboard", byte_count, byte_count == 1 ? "" : "s");
        return;
    }
    ui_error("clipboard copy failed: %s", error ? error : "unknown error");
}

/* ---------- task and session status ---------- */

#define SESSION_LABEL_WIDTH 14

/* Indent of value rows; also decides between the aligned label column and stacked layout. */
static int session_value_indent(int columns)
{
    int value_column = 2 + SESSION_LABEL_WIDTH;
    return columns - value_column >= UI_ROW_MIN_TEXT_CELLS ? value_column : UI_ROW_STACKED_INDENT;
}

static void print_session_row(const char *label, const char *value)
{
    ui_label_row(label, ANSI_DIM, value, ANSI_DIM, 2 + SESSION_LABEL_WIDTH, display_width());
}

/* Unknown and negligible cost estimates are omitted. The returned length may exceed the buffer. */
static int append_token_segment(char *row, size_t row_size, int row_length, const char *label,
                                long tokens, double cost)
{
    char formatted[32];
    if (row_length < 0 || (size_t)row_length >= row_size)
        return row_length;
    format_tokens(formatted, sizeof(formatted), tokens);
    row_length += snprintf(row + row_length, row_size - (size_t)row_length, "%s%s %s",
                           row_length ? " · " : "", label, formatted);
    if (cost >= COST_DISPLAY_MIN && row_length > 0 && (size_t)row_length < row_size) {
        format_cost(formatted, sizeof(formatted), cost);
        row_length += snprintf(row + row_length, row_size - (size_t)row_length, " ~%s", formatted);
    }
    return row_length;
}

static void kill_tasks(const char *arguments)
{
    const char **ids = NULL;
    size_t id_count = 0;
    size_t id_capacity = 0;
    char *words = xstrdup(arguments);
    int all = 0;
    for (char *word = strtok(words, " \t"); word; word = strtok(NULL, " \t")) {
        if (strcmp(word, "all") == 0) {
            all = 1;
            continue;
        }
        if (id_count == id_capacity) {
            id_capacity = id_capacity ? id_capacity * 2 : 4;
            ids = xrealloc(ids, id_capacity * sizeof(*ids));
        }
        ids[id_count++] = word;
    }
    if (!all && id_count == 0) {
        ui_error("usage: /tasks kill <id>... | kill all");
    } else {
        size_t stopped = task_stop(all ? NULL : ids, all ? 0 : id_count);
        printf("  stopped %zu task%s\n", stopped, stopped == 1 ? "" : "s");
    }
    free(ids);
    free(words);
}

static void run_tasks(const struct command_call *call)
{
    if (config_bool("no_tasks")) {
        ui_note("background tasks are disabled (no_tasks)");
        return;
    }
    const char *argument = call->argument;
    if (argument && *argument) {
        if (strncmp(argument, "kill", 4) == 0 &&
            (argument[4] == '\0' || argument[4] == ' ' || argument[4] == '\t'))
            kill_tasks(argument + 4);
        else
            ui_error("usage: /tasks [kill <id>... | kill all]");
        return;
    }

    struct task_info *tasks = NULL;
    size_t task_count = task_list(&tasks);
    if (task_count == 0) {
        printf("  " ANSI_DIM "no background tasks" ANSI_RESET "\n");
        free(tasks);
        return;
    }

    struct task_status {
        char text[40];
    } *statuses = xmalloc(task_count * sizeof(*statuses));
    int terminal_width = display_width();
    int id_width = 4;
    int status_width = 0;
    for (size_t i = 0; i < task_count; i++) {
        int id_cells = (int)strlen(tasks[i].id);
        if (id_cells > id_width)
            id_width = id_cells;
        char state_label[16];
        char elapsed_label[16];
        if (tasks[i].running)
            snprintf(state_label, sizeof(state_label), "running");
        else if (tasks[i].term_signal)
            snprintf(state_label, sizeof(state_label), "signal %d", tasks[i].term_signal);
        else
            snprintf(state_label, sizeof(state_label), "exit %d", tasks[i].exit_code);
        format_duration(elapsed_label, sizeof(elapsed_label), tasks[i].elapsed_ms);
        snprintf(statuses[i].text, sizeof(statuses[i].text), "%s · %s", state_label, elapsed_label);
        int status_cells = (int)display_cells(statuses[i].text);
        if (status_cells > status_width)
            status_width = status_cells;
    }
    for (size_t i = 0; i < task_count; i++) {
        int status_padding = status_width - (int)display_cells(statuses[i].text);
        int fixed_width = 2 + id_width + 2 + status_width + 2;
        int command_width = terminal_width - fixed_width - 1;
        if (command_width < 8)
            command_width = 8;
        char *flattened = flatten_for_display(tasks[i].command);
        char *command = truncate_for_display(flattened, (size_t)command_width);
        free(flattened);
        printf("  " ANSI_BOLD "%-*s" ANSI_BOLD_OFF "  %s%*s  " ANSI_DIM "%s" ANSI_RESET "\n",
               id_width, tasks[i].id, statuses[i].text, status_padding, "", command);
        free(command);
    }
    free(statuses);
    free(tasks);
}

/* Stats cover the current process: /new resets them and /resume does not restore old totals. */
static void run_session(const struct command_call *call)
{
    struct agent_state *state = call->state;
    const struct session_stats *stats = &state->stats;
    char row[160], formatted[32];

    const char *hint = session_log_resume_hint(state->session_log);
    print_session_row("session", hint ? hint : "not recorded");

    const char *preset = config_str("preset");
    if (preset && *preset)
        print_session_row("preset", preset);

    /* Report the effort the next request will carry after metadata resolution. */
    agent_session_resync_effort(state->session, state->provider, NULL);
    const char *provider_name =
        (state->provider && state->provider->name) ? state->provider->name : "?";
    const char *model = (state->session && state->session->model && *state->session->model)
                            ? state->session->model
                            : "?";
    const char *effort = state->session ? state->session->effort : NULL;
    if (effort && *effort)
        snprintf(row, sizeof(row), "%s · %s · %s", provider_name, model, effort);
    else
        snprintf(row, sizeof(row), "%s · %s", provider_name, model);
    /* When the identity overflows its row, break after the provider rather than between model
     * and effort; a hard newline in the value forces the row break. */
    int columns = display_width();
    if ((int)display_cells(row) > columns - session_value_indent(columns)) {
        if (effort && *effort)
            snprintf(row, sizeof(row), "%s\n%s · %s", provider_name, model, effort);
        else
            snprintf(row, sizeof(row), "%s\n%s", provider_name, model);
    }
    print_session_row("provider", row);

    if (stats->user_turns > 0) {
        snprintf(row, sizeof(row), "%ld", stats->user_turns);
        print_session_row("user turns", row);
    }

    if (stats->requests > 0) {
        snprintf(row, sizeof(row), "%ld", stats->requests);
        print_session_row("requests", row);
    }

    if (stats->tool_calls > 0) {
        int row_length = snprintf(row, sizeof(row), "%ld", stats->tool_calls);
        for (size_t i = 0; i < SESSION_STATS_MAX_TOOLS && stats->tools[i].name; i++) {
            if (row_length < 0 || (size_t)row_length >= sizeof(row))
                break;
            row_length += snprintf(row + row_length, sizeof(row) - (size_t)row_length, " · %s %ld",
                                   stats->tools[i].name, stats->tools[i].count);
        }
        print_session_row("tool calls", row);
    }

    if (stats->worked_ms > 0) {
        format_duration(formatted, sizeof(formatted), stats->worked_ms);
        print_session_row("time worked", formatted);
    }

    /* Context is the latest request's window use. Until a request reports usage — a fresh
     * session, or a compaction or history cut invalidated the snapshot — usage is unknown
     * rather than zero, but the resolved window is still worth showing. */
    if (stats->latest_context_tokens > 0) {
        format_context(row, sizeof(row), stats->latest_context_tokens, stats->context_limit);
        print_session_row("context", row);
    } else {
        long window =
            model_meta_context(state->provider, state->session ? state->session->model : NULL);
        if (window > 0) {
            format_context(row, sizeof(row), -1, window);
            print_session_row("context", row);
        }
    }

    /* Category costs are rate estimates even when the provider reported an exact total charge. */
    if (stats->input_tokens > 0 || stats->output_tokens > 0) {
        struct catalog_split split;
        int split_available = agent_spend_split(&stats->spend, &split);
        long cached_tokens = stats->cached_tokens > 0 ? stats->cached_tokens : 0;
        long cache_write_tokens = stats->cache_write_tokens > 0 ? stats->cache_write_tokens : 0;
        int row_length = 0;
        row_length =
            append_token_segment(row, sizeof(row), row_length, "in", stats->uncached_input_tokens,
                                 split_available ? split.cost_input : -1);
        if (cached_tokens > 0)
            row_length = append_token_segment(row, sizeof(row), row_length, "cache", cached_tokens,
                                              split_available ? split.cost_cache_read : -1);
        if (cache_write_tokens > 0)
            row_length =
                append_token_segment(row, sizeof(row), row_length, "write", cache_write_tokens,
                                     split_available ? split.cost_cache_write : -1);
        append_token_segment(row, sizeof(row), row_length, "out", stats->output_tokens,
                             split_available ? split.cost_output : -1);
        print_session_row("tokens total", row);
    }

    /* A mixed reported/estimated total remains an estimate. */
    int estimated = 0;
    double spend = agent_session_spend(stats, &estimated);
    if (spend > 0) {
        format_cost(formatted, sizeof(formatted), spend);
        snprintf(row, sizeof(row), "%s%s", estimated ? "~" : "", formatted);
        print_session_row("spend", row);
    }
}

/* ---------- /usage ---------- */

static void run_usage(const struct command_call *call)
{
    struct provider *provider = call->state->provider;
    if (!provider) {
        ui_note("no provider selected — use /provider to choose one first");
        return;
    }
    if (!provider->query_usage) {
        ui_note("/usage is not supported by the %s provider",
                provider->name ? provider->name : "?");
        return;
    }
    provider->query_usage(provider);
}

/* ---------- /login, /logout ---------- */

static void run_login(const struct command_call *call)
{
    login_command(call->state, call->argument);
}

static void run_logout(const struct command_call *call)
{
    logout_command(call->state, call->argument);
}

/* ---------- /help ---------- */

static void print_help_row(const char *label, const char *label_color, const char *summary,
                           int dimmed, int description_column, int columns)
{
    ui_label_row(label, label_color, summary, dimmed ? ANSI_DIM : "", 2 + description_column,
                 columns);
}

static void print_command_row(const char *name, const char *summary, int dimmed,
                              int description_column, int columns)
{
    char *label = xasprintf("/%s", name);
    print_help_row(label, theme_open(dimmed ? THEME_CHROME_DIM : THEME_CHROME), summary, dimmed,
                   description_column, columns);
    free(label);
}

static void run_help(const struct command_call *call)
{
    (void)call;

    size_t label_width = 0;
    for (size_t i = 0; i < N_COMMANDS; i++) {
        size_t command_width = 1 + strlen(COMMANDS[i].name);
        if (command_width > label_width)
            label_width = command_width;
        if (COMMANDS[i].alias) {
            size_t alias_width = 1 + strlen(COMMANDS[i].alias);
            if (alias_width > label_width)
                label_width = alias_width;
        }
    }
    for (size_t i = 0; i < N_SHORTCUTS; i++) {
        size_t shortcut_width = strlen(SHORTCUTS[i].key);
        if (shortcut_width > label_width)
            label_width = shortcut_width;
    }
    int description_column = (int)label_width + 2;
    int columns = display_width();

    fputs(ANSI_BOLD "commands" ANSI_RESET "\n", stdout);
    for (size_t i = 0; i < N_COMMANDS; i++) {
        print_command_row(COMMANDS[i].name, COMMANDS[i].summary, 0, description_column, columns);
        if (COMMANDS[i].alias) {
            char *summary = xasprintf("alias for /%s", COMMANDS[i].name);
            print_command_row(COMMANDS[i].alias, summary, 1, description_column, columns);
            free(summary);
        }
    }

    fputc('\n', stdout);
    fputs(ANSI_BOLD "shortcuts" ANSI_RESET "\n", stdout);
    for (size_t i = 0; i < N_SHORTCUTS; i++) {
        int available = !SHORTCUTS[i].available || SHORTCUTS[i].available();
        const char *label_color = theme_open(available ? THEME_CHROME : THEME_CHROME_DIM);
        if (available) {
            print_help_row(SHORTCUTS[i].key, label_color, SHORTCUTS[i].description, 0,
                           description_column, columns);
        } else {
            char *summary =
                xasprintf("%s %s", SHORTCUTS[i].description, SHORTCUTS[i].unavailable_note);
            print_help_row(SHORTCUTS[i].key, label_color, summary, 1, description_column, columns);
            free(summary);
        }
    }
}
