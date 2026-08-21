/* SPDX-License-Identifier: MIT */
#include "providers/usage_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "render/ctrl_strip.h"
#include "render/progress.h"
#include "terminal/ansi.h"
#include "terminal/width.h"

#define USAGE_BAR_WIDTH  20
#define USAGE_BAR_COLUMN (2 + USAGE_LABEL_WIDTH + 1)

static void format_reset_time(char *output, size_t output_size, time_t reset_at)
{
    time_t now = time(NULL);
    struct tm reset_tm, now_tm;
    if (!localtime_r(&reset_at, &reset_tm) || !localtime_r(&now, &now_tm)) {
        snprintf(output, output_size, "?");
        return;
    }

    int same_day = reset_tm.tm_year == now_tm.tm_year && reset_tm.tm_yday == now_tm.tm_yday;
    if (same_day)
        strftime(output, output_size, "%H:%M", &reset_tm);
    else
        /* Zero-padded %d is preferable to the non-portable %-d. */
        strftime(output, output_size, "%a %b %d, %H:%M", &reset_tm);
}

void usage_window_print(const struct usage_window *window)
{
    /* Label and note may come from a server response; keep terminal controls and line breaks
     * out of the row. */
    char *label = ctrl_strip_line_dup(window->label);
    char *note = window->note ? ctrl_strip_line_dup(window->note) : NULL;
    double used = window->used_percent;
    if (used < 0)
        used = 0;
    if (used > 100)
        used = 100;

    char reset_time[64];
    format_reset_time(reset_time, sizeof(reset_time), window->reset_at);

    char percent_text[32];
    int percent_width =
        snprintf(percent_text, sizeof(percent_text), " %3d%% used", (int)(used + 0.5));
    char reset_text[160];
    int reset_width;
    if (note)
        reset_width =
            snprintf(reset_text, sizeof(reset_text), " · resets %s · %s", reset_time, note);
    else
        reset_width = snprintf(reset_text, sizeof(reset_text), " · resets %s", reset_time);
    int row_width = USAGE_BAR_COLUMN + USAGE_BAR_WIDTH + percent_width + reset_width;

    printf("  " ANSI_DIM "%-*s" ANSI_RESET " ", USAGE_LABEL_WIDTH, label);
    progress_bar_print(used / 100.0, USAGE_BAR_WIDTH);
    if (row_width <= display_width()) {
        printf(ANSI_DIM "%s%s" ANSI_RESET "\n", percent_text, reset_text);
    } else {
        printf(ANSI_DIM "%s" ANSI_RESET "\n", percent_text);
        printf("%*s" ANSI_DIM "resets %s", USAGE_BAR_COLUMN, "", reset_time);
        if (note)
            printf(" · %s", note);
        printf(ANSI_RESET "\n");
    }

    free(note);
    free(label);
}
