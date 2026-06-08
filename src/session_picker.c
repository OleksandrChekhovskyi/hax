/* SPDX-License-Identifier: MIT */
#include "session_picker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "session.h"
#include "util.h"
#include "terminal/ansi.h"
#include "terminal/ui.h"

/* Sessions revealed per page, newest first. A printed list (not a
 * scrolling TUI), so 'm' prints the next page in place — each step is
 * bounded regardless of how many sessions a long-lived project has. Any
 * session is also reachable directly by id via --resume=ID. */
#define SESSION_PICKER_PAGE 10

/* read_choice sentinel: user asked for the next page ('m'). */
#define PICK_MORE (-2)

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

/* Read one line from stdin and parse a 1-based selection. Returns the
 * 0-based index in [0,count), PICK_MORE when the user types 'm', or -1
 * for cancel (empty line, EOF, 'q', out-of-range, or junk). */
static long read_choice(size_t count)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t len = getline(&line, &cap, stdin);
    if (len < 0) {
        free(line);
        return -1;
    }
    /* Trim trailing newline and surrounding spaces. */
    char *s = line;
    while (*s == ' ' || *s == '\t')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';

    long choice = -1;
    if (*s == 'm' || *s == 'M') {
        choice = PICK_MORE;
    } else if (*s && *s != 'q' && *s != 'Q') {
        char *parse_end;
        long v = strtol(s, &parse_end, 10);
        if (*parse_end == '\0' && v >= 1 && (size_t)v <= count)
            choice = v - 1;
    }
    free(line);
    return choice;
}

char *session_picker_run(const char *cwd, const char *exclude_path)
{
    /* The picker is interactive: it prints a list/prompt to stdout and
     * reads a selection from stdin. Require both to be terminals (matching
     * the cursor/history gating elsewhere) so `hax --resume >out` doesn't
     * write the menu into a file and block waiting on stdin. */
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return NULL;

    struct session_entry *list;
    size_t n;
    session_list(cwd, &list, &n);

    /* Build the display order, dropping the excluded (live) session. */
    size_t *idx = n ? xmalloc(n * sizeof(*idx)) : NULL;
    size_t shown = 0;
    for (size_t i = 0; i < n; i++) {
        if (exclude_path && list[i].path && strcmp(list[i].path, exclude_path) == 0)
            continue;
        idx[shown++] = i;
    }

    if (shown == 0) {
        ui_note("no past conversations in this directory");
        free(idx);
        session_list_free(list, n);
        return NULL;
    }

    time_t now = time(NULL);
    /* Reveal a page at a time, printing only each new batch (the numbering
     * keeps climbing), so a project with hundreds of sessions doesn't dump
     * a wall of text — the user types 'm' until they spot the one they
     * want, or cancels. */
    printf(ANSI_BOLD "resume a conversation" ANSI_RESET "\n");
    size_t printed = 0;
    long pick;
    for (;;) {
        size_t next = printed + SESSION_PICKER_PAGE;
        if (next > shown)
            next = shown;
        for (size_t k = printed; k < next; k++) {
            struct session_entry *e = &list[idx[k]];
            char when[24];
            rel_time((long)(now - e->mtime), when, sizeof(when));
            /* Read the first prompt only for rows we actually print. */
            if (!e->first_prompt)
                e->first_prompt = session_first_prompt(e->path);
            printf("  " ANSI_CYAN "%2zu" ANSI_RESET "  " ANSI_DIM "%-9s" ANSI_RESET "  %s\n", k + 1,
                   when, e->first_prompt ? e->first_prompt : "");
        }
        printed = next;
        int more = printed < shown;
        printf("\n" ANSI_DIM "select 1-%zu%s, or enter to cancel: " ANSI_RESET, printed,
               more ? ", m for more" : "");
        fflush(stdout);

        pick = read_choice(printed);
        if (pick == PICK_MORE) {
            if (more)
                continue; /* print the next batch */
            pick = -1;    /* nothing more to show — treat as cancel */
        }
        break;
    }
    char *path = (pick >= 0) ? xstrdup(list[idx[pick]].path) : NULL;

    free(idx);
    session_list_free(list, n);
    return path;
}
