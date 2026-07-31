/* SPDX-License-Identifier: MIT */
#include "session_picker.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "session.h"
#include "terminal/picker.h"
#include "terminal/ui.h"
#include "util.h"

/* Search farther than the visible row so filters can match later prompt text. */
#define SESSION_LABEL_CELLS 512

/* Bound startup work; older sessions remain addressable through --resume=<id>. */
#define SESSION_PICKER_MAX 200

static void format_relative_time(long seconds_ago, char *buffer, size_t size)
{
    if (seconds_ago < 0)
        seconds_ago = 0;
    if (seconds_ago < 60)
        snprintf(buffer, size, "just now");
    else if (seconds_ago < 3600)
        snprintf(buffer, size, "%ldm ago", seconds_ago / 60);
    else if (seconds_ago < 86400)
        snprintf(buffer, size, "%ldh ago", seconds_ago / 3600);
    else
        snprintf(buffer, size, "%ldd ago", seconds_ago / 86400);
}

char *session_picker_run(const char *cwd, const char *exclude_path, int *picker_opened)
{
    if (picker_opened)
        *picker_opened = 0;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return NULL;

    struct session_entry *entries;
    size_t entry_count;
    session_list(cwd, &entries, &entry_count);

    size_t *entry_indexes = entry_count ? xmalloc(entry_count * sizeof(*entry_indexes)) : NULL;
    size_t visible_count = 0;
    for (size_t i = 0; i < entry_count; i++) {
        if (exclude_path && entries[i].path && strcmp(entries[i].path, exclude_path) == 0)
            continue;
        entry_indexes[visible_count++] = i;
    }

    if (visible_count == 0) {
        ui_note("no past conversations in this directory");
        free(entry_indexes);
        session_list_free(entries, entry_count);
        return NULL;
    }

    size_t picker_count = visible_count < SESSION_PICKER_MAX ? visible_count : SESSION_PICKER_MAX;
    time_t now = time(NULL);
    struct picker_item *items = xcalloc(picker_count, sizeof(*items));
    char **details = xmalloc(picker_count * sizeof(*details));
    for (size_t i = 0; i < picker_count; i++) {
        struct session_entry *entry = &entries[entry_indexes[i]];
        if (!entry->first_prompt)
            entry->first_prompt = session_first_prompt(entry->path, SESSION_LABEL_CELLS);
        char relative_time[24];
        format_relative_time((long)(now - entry->mtime), relative_time, sizeof(relative_time));
        details[i] = xstrdup(relative_time);
        items[i].label =
            entry->first_prompt && entry->first_prompt[0] ? entry->first_prompt : "(no preview)";
        items[i].detail = details[i];
    }

    char counted_title[96];
    const char *title = "resume a conversation";
    if (picker_count < visible_count) {
        snprintf(counted_title, sizeof(counted_title), "resume a conversation · newest %zu of %zu",
                 picker_count, visible_count);
        title = counted_title;
    }

    struct picker_opts options = {
        .title = title,
        .items = items,
        .n = picker_count,
        .label_gutter = 1,
    };
    /* Even a raw-mode setup failure leaves the cursor at the picker's start row. */
    if (picker_opened)
        *picker_opened = 1;
    long selection = picker_run(&options);
    char *path = NULL;
    if (selection >= 0 && (size_t)selection < picker_count)
        path = xstrdup(entries[entry_indexes[selection]].path);

    for (size_t i = 0; i < picker_count; i++)
        free(details[i]);
    free(details);
    free(items);
    free(entry_indexes);
    session_list_free(entries, entry_count);
    return path;
}
