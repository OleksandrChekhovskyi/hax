/* SPDX-License-Identifier: MIT */
#include "slash.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "agent_core.h"
#include "buf.h"
#include "config.h"
#include "file_mention.h"
#include "provider.h"
#include "select.h"
#include "session.h"
#include "session_picker.h"
#include "xalloc.h"
#include "commands/login.h"
#include "commands/session_cmd.h"
#include "commands/tasks.h"
#include "render/disp.h"
#include "render/render_ctx.h"
#include "terminal/ansi.h"
#include "terminal/clipboard.h"
#include "terminal/input_core.h"
#include "terminal/picker.h"
#include "terminal/theme.h"
#include "terminal/ui.h"
#include "terminal/width.h"
#include "text/completion.h"
#include "text/display_safe.h"
#include "text/width.h"

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
    const char *usage; /* argument placeholder such as "[preset]"; NULL takes no argument */
    enum command_display display;
    void (*handler)(const struct command_call *call);
    /* Add the values of the argument word that follows `preceding`, the trimmed earlier
     * arguments. Runs on Tab, so it must return promptly: no network, child processes, waiting on
     * background work, or tty output. NULL completes nothing. */
    void (*argument_choices)(struct agent_state *state, const char *preceding,
                             struct completion *choices);
    /* Return the placeholder for the argument word after `preceding`, the trimmed earlier
     * arguments, or NULL; `usage` already covers the first word. A NULL hook hints nothing
     * past the first word. */
    const char *(*later_usage)(const char *preceding);
};

struct shortcut {
    const char *key;
    const char *description;
    int (*available)(void);
    const char *unavailable_note;
};

struct parsed_command {
    char *name;
    char *argument; /* trimmed; NULL when absent */
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
static void provider_id_choices(struct agent_state *state, const char *preceding,
                                struct completion *choices);
static void model_id_choices(struct agent_state *state, const char *preceding,
                             struct completion *choices);
static void effort_level_choices(struct agent_state *state, const char *preceding,
                                 struct completion *choices);
static void preset_name_choices(struct agent_state *state, const char *preceding,
                                struct completion *choices);
static void preset_save_choices(struct agent_state *state, const char *preceding,
                                struct completion *choices);
static const char *preset_save_later_usage(const char *preceding);
static void config_choices(struct agent_state *state, const char *preceding,
                           struct completion *choices);
static const char *config_later_usage(const char *preceding);
static void tasks_argument_choices(struct agent_state *state, const char *preceding,
                                   struct completion *choices);
static const char *tasks_later_usage(const char *preceding);
static void login_provider_choices(struct agent_state *state, const char *preceding,
                                   struct completion *choices);
static void logout_provider_choices(struct agent_state *state, const char *preceding,
                                    struct completion *choices);

/* Registry order is also /help order. */
static const struct slash_command COMMANDS[] = {
    {
        .name = "new",
        .alias = "clear",
        .summary = "start a fresh conversation",
        .usage = "[preset]",
        .handler = run_new,
        .argument_choices = preset_name_choices,
    },
    {
        .name = "resume",
        .summary = "resume a past conversation",
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_resume,
    },
    {
        .name = "undo",
        .summary = "revert conversation to before an earlier message",
        .usage = "[turns back]",
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_undo,
    },
    {
        .name = "fork",
        .summary = "branch a new session before an earlier message",
        .usage = "[turns back]",
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_fork,
    },
    {
        .name = "provider",
        .summary = "switch provider",
        .usage = "[id]",
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_provider,
        .argument_choices = provider_id_choices,
    },
    {
        .name = "model",
        .summary = "switch model",
        .usage = "[id]",
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_model,
        .argument_choices = model_id_choices,
    },
    {
        .name = "effort",
        .summary = "set reasoning effort",
        .usage = "[level]",
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_effort,
        .argument_choices = effort_level_choices,
    },
    {
        .name = "preset",
        .summary = "switch to a config-defined preset",
        .usage = "[name]",
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_preset,
        .argument_choices = preset_name_choices,
    },
    /* `/preset save` would conflict with a preset named "save". */
    {
        .name = "preset-save",
        .summary = "save the current selection as a preset",
        .usage = "<name> [tint]",
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_preset_save,
        .argument_choices = preset_save_choices,
        .later_usage = preset_save_later_usage,
    },
    {
        .name = "config",
        .summary = "view or change settings",
        .usage = "[key [value]]",
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_config,
        .argument_choices = config_choices,
        .later_usage = config_later_usage,
    },
    {
        .name = "compact",
        .summary = "summarize the conversation to free up context",
        .usage = "[focus]",
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
        .summary = "list background tasks",
        .usage = "[kill <id>... | kill all]",
        .handler = run_tasks,
        .argument_choices = tasks_argument_choices,
        .later_usage = tasks_later_usage,
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
        .summary = "log in to a provider account, managed by hax",
        .usage = "[provider]",
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_login,
        .argument_choices = login_provider_choices,
    },
    {
        .name = "logout",
        .summary = "log out and remove a hax-managed login",
        .usage = "[provider]",
        .display = COMMAND_DISPLAY_MANAGED,
        .handler = run_logout,
        .argument_choices = logout_provider_choices,
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
    {.key = "ctrl-o", .description = "view the conversation in $PAGER"},
    {.key = "ctrl-t", .description = "view model-facing transcript in $PAGER"},
    {.key = "ctrl-v", .description = "paste image (or text) from clipboard"},
    {.key = "@ + tab",
     .description = "pick a project file to mention",
     .available = file_mention_available,
     .unavailable_note = "(fzf not installed)"},
};
#define N_SHORTCUTS (sizeof(SHORTCUTS) / sizeof(SHORTCUTS[0]))

static int is_name_byte(unsigned char c)
{
    return isalnum(c) || c == '_' || c == '-';
}

static int parse_command(const char *line, struct parsed_command *parsed)
{
    if (!line || line[0] != '/')
        return 0;

    const char *name = line + 1;
    const char *cursor = name;
    while (*cursor && !isspace((unsigned char)*cursor)) {
        /* Restrict command-shaped input so paths and terminal control bytes pass through. */
        if (!is_name_byte((unsigned char)*cursor))
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
    size_t argument_length = strlen(cursor);
    while (argument_length > 0 && isspace((unsigned char)cursor[argument_length - 1]))
        argument_length--;
    parsed->argument = argument_length ? xasprintf("%.*s", (int)argument_length, cursor) : NULL;
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

/* Look up the command spelled by the first `name_len` bytes of `text`. */
static const struct slash_command *find_command_span(const char *text, size_t name_len)
{
    char *name = xasprintf("%.*s", (int)name_len, text);
    const struct slash_command *command = find_command(name);
    free(name);
    return command;
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
    if (parsed.argument && !command->usage) {
        ui_error("/%s takes no arguments.", parsed.name);
        result = SLASH_BAD_USAGE;
        goto raw_output;
    }

    struct command_call call = {
        .state = state,
        .argument = command->usage ? parsed.argument : NULL,
    };
    command->handler(&call);
    if (command->display == COMMAND_DISPLAY_RAW)
        disp_sync_external_line(disp);
    free(parsed.argument);
    free(parsed.name);
    return SLASH_HANDLED;

raw_output:
    disp_sync_external_line(disp);
    free(parsed.argument);
    free(parsed.name);
    return result;
}

/* ---------- completion and prompt hints ---------- */

/* The command name occupies [1, name_end) of a line that starts with a slash. Return 0 for input
 * that is not command-shaped, such as a path, so no completion or hint applies. */
static int scan_command_name(const char *line, size_t *name_end)
{
    if (line[0] != '/')
        return 0;

    size_t end = 1;
    while (is_name_byte((unsigned char)line[end]))
        end++;
    if (line[end] != '\0' && !isspace((unsigned char)line[end]))
        return 0;
    *name_end = end;
    return 1;
}

/* `text` is the command line after its slash, up to the cursor. Collect the choices matching its
 * last word, which `*word` points at; that word is the command name when no space precedes it. */
static void collect_choices(struct agent_state *state, const char *text, struct completion *choices,
                            const char **word)
{
    const char *word_start = text + strlen(text);
    while (word_start > text && !isspace((unsigned char)word_start[-1]))
        word_start--;
    *word = word_start;

    if (word_start == text) {
        for (size_t i = 0; i < N_COMMANDS; i++) {
            completion_add(choices, COMMANDS[i].name);
            if (COMMANDS[i].alias)
                completion_add(choices, COMMANDS[i].alias);
        }
    } else {
        size_t name_len = 0;
        while (is_name_byte((unsigned char)text[name_len]))
            name_len++;
        const struct slash_command *command = find_command_span(text, name_len);
        if (!command || !command->argument_choices)
            return;

        const char *preceding = text + name_len;
        while (isspace((unsigned char)*preceding))
            preceding++;
        size_t preceding_len = (size_t)(word_start - preceding);
        while (preceding_len > 0 && isspace((unsigned char)preceding[preceding_len - 1]))
            preceding_len--;
        char *trimmed = xasprintf("%.*s", (int)preceding_len, preceding);
        command->argument_choices(state, trimmed, choices);
        free(trimmed);
    }
    completion_keep_prefixed(choices, word_start);
}

/* Complete only the word that ends at the cursor, on the line's first row. */
static int match_command(const char *buffer, size_t buffer_len, size_t cursor, size_t *start,
                         size_t *end, void *user)
{
    (void)user;

    size_t name_end;
    if (cursor > buffer_len || !scan_command_name(buffer, &name_end) || cursor < name_end)
        return 0;
    if (cursor < buffer_len && !isspace((unsigned char)buffer[cursor]))
        return 0;
    if (memchr(buffer, '\n', cursor))
        return 0;
    /* Leave other argument words, such as an @file mention in a /compact focus, to later
     * completers. */
    if (cursor > name_end) {
        const struct slash_command *command = find_command_span(buffer + 1, name_end - 1);
        if (!command || !command->argument_choices)
            return 0;
    }
    *start = 1;
    *end = cursor;
    return 1;
}

static char *complete_command(const char *text, void *user)
{
    struct completion choices = {0};
    const char *word;
    char *replacement = NULL;

    collect_choices(user, text, &choices, &word);
    char *extended = completion_extend(&choices, word);
    if (extended)
        replacement = xasprintf("%.*s%s", (int)(word - text), text, extended);
    free(extended);
    completion_free(&choices);
    return replacement;
}

/* Two spaces set the list apart from the text it follows. A bare slash matches every command,
 * which /help already lists in full instead of a truncated row. Like a shell listing a directory,
 * candidates that split into parts show only their next part. */
static char *list_command_choices(const char *text, void *user)
{
    if (*text == '\0')
        return xstrdup("  see /help");

    struct completion choices = {0};
    const char *word;
    char *listing = NULL;

    collect_choices(user, text, &choices, &word);
    completion_to_parts(&choices, word);
    if (choices.count > 1) {
        const char *marker = word == text ? "/" : "";
        struct buf list;
        buf_init(&list);
        for (size_t i = 0; i < choices.count; i++) {
            buf_append_str(&list, i > 0 ? " " : "  ");
            buf_append_str(&list, marker);
            buf_append_str(&list, choices.candidates[i]);
        }
        listing = buf_steal(&list);
    }
    completion_free(&choices);
    return listing;
}

void slash_completer_init(struct input_completer *completer, struct agent_state *state)
{
    *completer = (struct input_completer){
        .match = match_command,
        .complete = complete_command,
        .candidates = list_command_choices,
        .user = state,
    };
}

/* Placeholders appear once the name is complete and before any argument, so a mistyped or
 * partial name draws nothing. A later argument's appears once a space ends the word before it. */
char *slash_hint(const char *line)
{
    size_t name_end;

    if (!scan_command_name(line, &name_end) || strchr(line, '\n'))
        return NULL;

    const struct slash_command *command = find_command_span(line + 1, name_end - 1);
    if (!command || !command->usage)
        return NULL;

    const char *argument = line + name_end;
    while (isspace((unsigned char)*argument))
        argument++;
    if (!*argument)
        return xasprintf("%s%s", line[name_end] == '\0' ? " " : "", command->usage);

    size_t argument_len = strlen(argument);
    if (!command->later_usage || !isspace((unsigned char)argument[argument_len - 1]))
        return NULL;
    while (isspace((unsigned char)argument[argument_len - 1]))
        argument_len--;
    char *preceding = xasprintf("%.*s", (int)argument_len, argument);
    const char *usage = command->later_usage(preceding);
    free(preceding);
    return usage ? xstrdup(usage) : NULL;
}

/* Whether trimmed `arguments` hold exactly one word. */
static int is_one_word(const char *arguments)
{
    if (!*arguments)
        return 0;
    for (const char *cursor = arguments; *cursor; cursor++)
        if (isspace((unsigned char)*cursor))
            return 0;
    return 1;
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
        /* N counts user turns back from the end: 1 is the most recent, `turn_count`
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
    select_provider(call->state, call->argument);
}

static void provider_id_choices(struct agent_state *state, const char *preceding,
                                struct completion *choices)
{
    (void)state;
    if (!*preceding)
        select_provider_choices(choices);
}

static void run_model(const struct command_call *call)
{
    select_model(call->state, call->argument);
}

static void model_id_choices(struct agent_state *state, const char *preceding,
                             struct completion *choices)
{
    if (!*preceding)
        select_model_choices(state, choices);
}

static void run_effort(const struct command_call *call)
{
    select_effort(call->state, call->argument);
}

static void effort_level_choices(struct agent_state *state, const char *preceding,
                                 struct completion *choices)
{
    if (!*preceding)
        select_effort_choices(state, choices);
}

static void run_preset(const struct command_call *call)
{
    select_preset(call->state, call->argument, 1);
}

/* Alphabetical, like the preset picker. Only names in the preset-name grammar are offered: a
 * hand-written name outside it, such as one with a space, cannot complete as one word. */
static void preset_name_choices(struct agent_state *state, const char *preceding,
                                struct completion *choices)
{
    (void)state;
    if (*preceding)
        return;

    char **names = NULL;
    size_t count = config_preset_names(&names);
    for (size_t i = 0; i < count; i++) {
        if (config_preset_name_valid(names[i]))
            completion_add(choices, names[i]);
        free(names[i]);
    }
    free(names);
    completion_sort(choices);
}

static void run_preset_save(const struct command_call *call)
{
    select_preset_save(call->state, call->argument);
}

/* Naming an existing preset overwrites it after confirmation. */
static void preset_save_choices(struct agent_state *state, const char *preceding,
                                struct completion *choices)
{
    if (!*preceding)
        preset_name_choices(state, preceding, choices);
    else if (is_one_word(preceding))
        select_tint_choices(choices);
}

static const char *preset_save_later_usage(const char *preceding)
{
    return is_one_word(preceding) ? "[tint]" : NULL;
}

static void run_config(const struct command_call *call)
{
    select_config(call->state, call->argument);
}

static void config_choices(struct agent_state *state, const char *preceding,
                           struct completion *choices)
{
    (void)state;
    if (!*preceding)
        select_config_key_choices(choices);
    else if (is_one_word(preceding))
        select_config_value_choices(preceding, choices);
}

static const char *config_later_usage(const char *preceding)
{
    if (!is_one_word(preceding))
        return NULL;
    const struct config_setting *setting = config_setting_find(preceding);
    return setting && setting->editable ? "[value]" : NULL;
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

/* ---------- /tasks, /session ---------- */

static void tasks_argument_choices(struct agent_state *state, const char *preceding,
                                   struct completion *choices)
{
    (void)state;
    tasks_choices(preceding, choices);
}

static const char *tasks_later_usage(const char *preceding)
{
    return strcmp(preceding, "kill") == 0 ? "<id>... | all" : NULL;
}

static void run_tasks(const struct command_call *call)
{
    tasks_command(call->argument);
}

static void run_session(const struct command_call *call)
{
    session_command(call->state);
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

static void login_provider_choices(struct agent_state *state, const char *preceding,
                                   struct completion *choices)
{
    (void)state;
    if (!*preceding)
        login_choices(choices);
}

static void logout_provider_choices(struct agent_state *state, const char *preceding,
                                    struct completion *choices)
{
    (void)state;
    if (!*preceding)
        logout_choices(choices);
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

static char *command_help_summary(const struct slash_command *command)
{
    if (!command->usage)
        return xstrdup(command->summary);
    return xasprintf("%s %s", command->summary, command->usage);
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
        char *summary = command_help_summary(&COMMANDS[i]);
        print_command_row(COMMANDS[i].name, summary, 0, description_column, columns);
        free(summary);
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
