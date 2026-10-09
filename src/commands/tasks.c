/* SPDX-License-Identifier: MIT */
#include "commands/tasks.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "xalloc.h"
#include "terminal/ansi.h"
#include "terminal/ui.h"
#include "terminal/width.h"
#include "text/completion.h"
#include "text/display_safe.h"
#include "text/fmt.h"
#include "text/width.h"
#include "tools/task_registry.h"

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

/* Return the arguments after a leading "kill" word, or NULL when `arguments` start otherwise. */
static const char *kill_arguments(const char *arguments)
{
    if (strncmp(arguments, "kill", 4) != 0 ||
        (arguments[4] != '\0' && arguments[4] != ' ' && arguments[4] != '\t'))
        return NULL;
    return arguments + 4;
}

static int names_word(const char *words, const char *word)
{
    size_t word_len = strlen(word);
    for (const char *cursor = words; *cursor;) {
        while (isspace((unsigned char)*cursor))
            cursor++;
        const char *end = cursor;
        while (*end && !isspace((unsigned char)*end))
            end++;
        if ((size_t)(end - cursor) == word_len && strncmp(cursor, word, word_len) == 0)
            return 1;
        cursor = end;
    }
    return 0;
}

void tasks_choices(const char *preceding, struct completion *choices)
{
    if (config_bool("no_tasks"))
        return;
    if (!*preceding) {
        completion_add(choices, "kill");
        return;
    }
    const char *named = kill_arguments(preceding);
    if (!named || names_word(named, "all"))
        return;
    if (!*named)
        completion_add(choices, "all");

    struct task_info *tasks = NULL;
    size_t task_count = task_list(&tasks);
    for (size_t i = 0; i < task_count; i++) {
        if (tasks[i].running && !names_word(named, tasks[i].id))
            completion_add(choices, tasks[i].id);
    }
    free(tasks);
}

void tasks_command(const char *argument)
{
    if (config_bool("no_tasks")) {
        ui_note("background tasks are disabled (no_tasks)");
        return;
    }
    if (argument && *argument) {
        const char *named = kill_arguments(argument);
        if (named)
            kill_tasks(named);
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
