/* SPDX-License-Identifier: MIT */
#include "session_picker.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "session.h"
#include "util.h"
#include "terminal/picker.h"
#include "terminal/ui.h"

/* Search/display budget for the first-prompt label. The picker clips it to
 * the row width for display; the extra reach lets a filter match terms well
 * past the on-screen preview, not just its first few words. */
#define SESSION_LABEL_CELLS 512

/* Cap on how many sessions the picker reads before first paint (newest
 * first). Each row reads a bounded transcript prefix, so without a cap a
 * repo with thousands of sessions would stall the picker on open. Older
 * sessions stay reachable by id via `--resume=<id>`. */
#define SESSION_PICKER_MAX 200

/* Coarse "3m ago" / "2d ago" stamp from a delta in seconds. Good enough
 * to disambiguate sessions in a picker; not a precise clock. */
static void rel_time(long secs_ago, char *buf, size_t n)
{
    if (secs_ago < 0)
        secs_ago = 0;
    if (secs_ago < 60)
        snprintf(buf, n, "just now");
    else if (secs_ago < 3600)
        snprintf(buf, n, "%ldm ago", secs_ago / 60);
    else if (secs_ago < 86400)
        snprintf(buf, n, "%ldh ago", secs_ago / 3600);
    else
        snprintf(buf, n, "%ldd ago", secs_ago / 86400);
}

char *session_picker_run(const char *cwd, const char *exclude_path, int *shown)
{
    if (shown)
        *shown = 0;

    /* The picker is interactive (raw-mode list + stdin selection); require
     * both ends to be terminals so `hax --resume >out` doesn't paint a menu
     * into a file and block waiting on stdin. picker_run re-checks this, but
     * gating here lets us skip the whole list build on the non-tty path. */
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return NULL;

    struct session_entry *list;
    size_t n;
    session_list(cwd, &list, &n);

    /* Build the display order, dropping the excluded (live) session. */
    size_t *map = n ? xmalloc(n * sizeof(*map)) : NULL;
    size_t n_shown = 0;
    for (size_t i = 0; i < n; i++) {
        if (exclude_path && list[i].path && strcmp(list[i].path, exclude_path) == 0)
            continue;
        map[n_shown++] = i;
    }

    if (n_shown == 0) {
        ui_note("no past conversations in this directory");
        free(map);
        session_list_free(list, n);
        return NULL;
    }

    /* Materialize a row per session, newest first: the first prompt is the
     * searchable label (the picker clips it to the row width for display),
     * the relative time a dim detail column. Reads are capped at
     * SESSION_PICKER_MAX so opening the picker never stalls on a huge
     * history; the title notes when the list is capped. */
    size_t n_load = n_shown < SESSION_PICKER_MAX ? n_shown : SESSION_PICKER_MAX;
    time_t now = time(NULL);
    struct picker_item *items = xmalloc(n_load * sizeof(*items));
    char **details = xmalloc(n_load * sizeof(*details));
    for (size_t k = 0; k < n_load; k++) {
        struct session_entry *e = &list[map[k]];
        if (!e->first_prompt)
            e->first_prompt = session_first_prompt(e->path, SESSION_LABEL_CELLS);
        char when[24];
        rel_time((long)(now - e->mtime), when, sizeof(when));
        details[k] = xstrdup(when);
        items[k].label = (e->first_prompt && e->first_prompt[0]) ? e->first_prompt : "(no preview)";
        items[k].detail = details[k];
    }

    char title_buf[96];
    const char *title = "resume a conversation";
    if (n_load < n_shown) {
        snprintf(title_buf, sizeof title_buf, "resume a conversation · newest %zu of %zu", n_load,
                 n_shown);
        title = title_buf;
    }

    struct picker_opts opts = {
        .title = title,
        .items = items,
        .n = n_load,
        .empty_note = NULL,
    };
    /* Set before handing off (see session_picker.h): /resume's trail
     * bookkeeping only needs that the cursor ends at the picker's start row,
     * which holds even if picker_run's raw-mode setup fails without ever
     * painting — so this need not wait on a successful paint. */
    if (shown)
        *shown = 1;
    long sel = picker_run(&opts);
    char *path = (sel >= 0) ? xstrdup(list[map[sel]].path) : NULL;

    for (size_t k = 0; k < n_load; k++)
        free(details[k]);
    free(details);
    free(items);
    free(map);
    session_list_free(list, n);
    return path;
}
