/* SPDX-License-Identifier: MIT */
#include "progress.h"

#include <stdio.h>

#include "ansi.h"
#include "util.h"

/* Subtle look: the whole bar renders at ANSI_DIM with contrast coming
 * from glyph density alone — "█" full block for the fill, "░"
 * light-shade for the empty track. Whole-cell resolution: rounding
 * `frac * width` to the nearest cell keeps the boundary uniform with
 * the rest of the fill (sub-cell partial blocks like "▊" rendered at
 * the same intensity as "█" still read as visually distinct in most
 * terminals — too thin a glyph to blend cleanly). The intent is for
 * /usage and similar status output to recede behind conversation
 * text. No brackets either, for the same reason. */
void progress_bar_print(double frac, int width)
{
    if (frac < 0)
        frac = 0;
    if (frac > 1)
        frac = 1;
    if (width < 1)
        width = 1;

    int filled = (int)(frac * width + 0.5);

    fputs(ANSI_DIM, stdout);
    if (locale_have_utf8()) {
        for (int i = 0; i < filled; i++)
            fputs("\xe2\x96\x88", stdout); /* █ full block */
        for (int i = filled; i < width; i++)
            fputs("\xe2\x96\x91", stdout); /* ░ light shade */
    } else {
        for (int i = 0; i < filled; i++)
            fputc('#', stdout);
        for (int i = filled; i < width; i++)
            fputc('-', stdout);
    }
    /* Close DIM specifically (SGR 22) instead of ANSI_RESET so any
     * caller-level attributes (e.g. an outer color span around a row
     * that contains the bar) survive. */
    fputs(ANSI_BOLD_OFF, stdout);
}
