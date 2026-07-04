/* SPDX-License-Identifier: MIT */
#include "terminal/picker_core.h"

#include <ctype.h>

/* ---------------- pure filter ---------------- */

/* Case-insensitive "does `hay` contain `needle`" over ASCII case. Bytes
 * >= 0x80 (UTF-8) compare verbatim, which is correct for the common
 * all-ASCII labels and degrades to case-sensitive for accented text —
 * acceptable for a filter, and avoids dragging locale casefolding in. */
static int contains_ci(const char *hay, const char *needle, size_t nlen)
{
    if (nlen == 0)
        return 1;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nlen)
            return 1;
    }
    return 0;
}

int picker_core_match(const char *text, const char *query)
{
    if (!query || !*query)
        return 1;
    if (!text)
        text = "";
    /* Split the query on runs of spaces; every term must be found. */
    const char *p = query;
    while (*p) {
        while (*p == ' ')
            p++;
        const char *start = p;
        while (*p && *p != ' ')
            p++;
        size_t len = (size_t)(p - start);
        if (len > 0 && !contains_ci(text, start, len))
            return 0;
    }
    return 1;
}

/* ---------------- selection / scroll state ---------------- */

static void clamp_scroll(struct picker_state *s)
{
    if (s->n_filtered == 0) {
        s->sel = 0;
        s->top = 0;
        return;
    }
    if (s->sel >= s->n_filtered)
        s->sel = s->n_filtered - 1;
    if (s->sel < s->top)
        s->top = s->sel;
    else if (s->sel >= s->top + (size_t)s->viewport)
        s->top = s->sel - (size_t)s->viewport + 1;
    /* Pull the window back when the list shrank under it. */
    if (s->n_filtered <= (size_t)s->viewport)
        s->top = 0;
    else if (s->top + (size_t)s->viewport > s->n_filtered)
        s->top = s->n_filtered - (size_t)s->viewport;
}

void picker_core_recompute(struct picker_state *s)
{
    const char *q = s->query.len ? s->query.data : "";
    s->n_filtered = 0;
    for (size_t i = 0; i < s->opts->n; i++) {
        if (picker_core_match(s->opts->items[i].label, q))
            s->filtered[s->n_filtered++] = i;
    }
    /* Land on the first match, not the old offset clamped into the new
     * list — after a query edit the previous position is meaningless (a
     * stale offset would make Enter grab an arbitrary match). The picker
     * applies its opts->initial positioning after this. */
    s->sel = 0;
    s->top = 0;
}

void picker_core_move_sel(struct picker_state *s, int delta)
{
    if (s->n_filtered == 0)
        return;
    if (delta < 0) {
        if (s->sel > 0)
            s->sel--;
    } else {
        if (s->sel + 1 < s->n_filtered)
            s->sel++;
    }
    clamp_scroll(s);
}

/* Park the window so the selection sits in the middle, clamped to the list
 * ends. Used after a page jump so the rows on either side of the new
 * selection stay visible — the user can then fine-tune with the arrows
 * without first scrolling to regain context. */
static void center_on_sel(struct picker_state *s)
{
    if (s->n_filtered <= (size_t)s->viewport) {
        s->top = 0;
        return;
    }
    size_t half = (size_t)s->viewport / 2;
    s->top = s->sel > half ? s->sel - half : 0;
    if (s->top + (size_t)s->viewport > s->n_filtered)
        s->top = s->n_filtered - (size_t)s->viewport;
}

void picker_core_page_sel(struct picker_state *s, int dir)
{
    if (s->n_filtered == 0)
        return;
    size_t step = (size_t)s->viewport / 2;
    if (step < 1)
        step = 1;
    if (dir < 0)
        s->sel = s->sel > step ? s->sel - step : 0;
    else
        s->sel = s->sel + step < s->n_filtered ? s->sel + step : s->n_filtered - 1;
    center_on_sel(s);
}

void picker_core_select_first(struct picker_state *s)
{
    s->sel = 0;
    clamp_scroll(s);
}

void picker_core_select_last(struct picker_state *s)
{
    s->sel = s->n_filtered ? s->n_filtered - 1 : 0;
    clamp_scroll(s);
}

void picker_core_select_item(struct picker_state *s, size_t item_idx)
{
    for (size_t fi = 0; fi < s->n_filtered; fi++) {
        if (s->filtered[fi] == item_idx) {
            s->sel = fi;
            center_on_sel(s);
            return;
        }
    }
}
