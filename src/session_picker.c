/* SPDX-License-Identifier: MIT */
#include "session_picker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "session.h"
#include "util.h"
#include "terminal/picker.h"
#include "terminal/ui.h"
#include "text/width.h"

/* Search farther than the visible row so filters can match later prompt text. */
#define SESSION_LABEL_CELLS 512

/* Keep the git line short enough that it rarely wraps onto a row the prompt needs. */
#define SESSION_SUBJECT_CELLS 60

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

static void append_field(struct buf *line, const char *text)
{
    if (!text || !*text)
        return;
    if (line->len)
        buf_append_str(line, " · ");
    buf_append_str(line, text);
}

static char *steal_or_free(struct buf *line)
{
    if (line->len)
        return buf_steal(line);
    buf_free(line);
    return NULL;
}

/* The startup banner's `[preset] provider · model · effort`, so a row reads the way the session
 * announced itself. */
static char *format_selection(const struct session_label *label)
{
    struct buf line;
    buf_init(&line);
    append_field(&line, label->provider);
    append_field(&line, label->model);
    append_field(&line, label->effort);

    char *fields = steal_or_free(&line);
    if (!label->preset || !*label->preset)
        return fields;
    char *stance =
        fields ? xasprintf("[%s] %s", label->preset, fields) : xasprintf("[%s]", label->preset);
    free(fields);
    return stance;
}

/* Where the work started, as "branch · commit subject". The hash the header also records is left
 * out: it distinguishes rows for no one reading them. */
static char *format_git(const struct session_label *label)
{
    struct buf line;
    buf_init(&line);
    append_field(&line, label->git_branch);
    char *subject = (label->git_subject && *label->git_subject)
                        ? truncate_for_display(label->git_subject, SESSION_SUBJECT_CELLS)
                        : NULL;
    append_field(&line, subject);
    free(subject);
    return steal_or_free(&line);
}

/* Returns NULL when the session recorded neither, which is what an unreadable file looks like. */
static char *format_provenance(const struct session_label *label)
{
    char *selection = format_selection(label);
    char *git = format_git(label);
    if (!selection)
        return git;
    if (!git)
        return selection;
    char *provenance = xasprintf("%s\n%s", selection, git);
    free(selection);
    free(git);
    return provenance;
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
    char **provenances = xmalloc(picker_count * sizeof(*provenances));
    for (size_t i = 0; i < picker_count; i++) {
        struct session_entry *entry = &entries[entry_indexes[i]];
        session_label_read(entry->path, SESSION_LABEL_CELLS, &entry->label);
        char relative_time[24];
        format_relative_time((long)(now - entry->mtime), relative_time, sizeof(relative_time));
        details[i] = xstrdup(relative_time);
        provenances[i] = format_provenance(&entry->label);
        const char *prompt = entry->label.prompt;
        items[i].label = prompt && prompt[0] ? prompt : "(no preview)";
        items[i].detail = details[i];
        items[i].description = provenances[i];
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
        .item_count = picker_count,
        .repeat_clipped_label = 1,
    };
    /* Even a raw-mode setup failure leaves the cursor at the picker's start row. */
    if (picker_opened)
        *picker_opened = 1;
    long selection = picker_run(&options);
    char *path = NULL;
    if (selection >= 0 && (size_t)selection < picker_count)
        path = xstrdup(entries[entry_indexes[selection]].path);

    for (size_t i = 0; i < picker_count; i++) {
        free(details[i]);
        free(provenances[i]);
    }
    free(provenances);
    free(details);
    free(items);
    free(entry_indexes);
    session_list_free(entries, entry_count);
    return path;
}
