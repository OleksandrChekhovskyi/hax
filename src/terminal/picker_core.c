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

int picker_core_row_enabled(const struct picker_state *s, size_t fi)
{
    return !s->opts->items[s->filtered[fi]].disabled;
}

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

/* Snap the selection onto an enabled row if it currently sits on a disabled
 * one: search forward from the current offset, then backward. Leaves the
 * selection put only when every filtered row is disabled (then Enter is a
 * no-op). Keeps the cursor off unselectable rows after a filter change or a
 * jump. */
static void ensure_enabled_sel(struct picker_state *s)
{
    if (s->n_filtered == 0 || picker_core_row_enabled(s, s->sel))
        return;
    for (size_t i = s->sel; i < s->n_filtered; i++) {
        if (picker_core_row_enabled(s, i)) {
            s->sel = i;
            clamp_scroll(s);
            return;
        }
    }
    for (size_t i = s->sel; i-- > 0;) {
        if (picker_core_row_enabled(s, i)) {
            s->sel = i;
            clamp_scroll(s);
            return;
        }
    }
}

void picker_core_recompute(struct picker_state *s)
{
    const char *q = s->query.len ? s->query.data : "";
    s->n_filtered = 0;
    for (size_t i = 0; i < s->opts->n; i++) {
        if (picker_core_match(s->opts->items[i].label, q))
            s->filtered[s->n_filtered++] = i;
    }
    clamp_scroll(s);
    ensure_enabled_sel(s);
}

void picker_core_move_sel(struct picker_state *s, int delta)
{
    if (s->n_filtered == 0)
        return;
    /* Step in `delta`'s direction to the next enabled row, skipping any
     * disabled ones. Stay put if there's no enabled row that way. */
    size_t cur = s->sel;
    for (;;) {
        if (delta < 0) {
            if (cur == 0)
                return;
            cur--;
        } else {
            if (cur + 1 >= s->n_filtered)
                return;
            cur++;
        }
        if (picker_core_row_enabled(s, cur)) {
            s->sel = cur;
            clamp_scroll(s);
            return;
        }
    }
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

/* Snap a disabled landing row onto an enabled one, preferring `dir`'s
 * direction so a page jump across a disabled block doesn't bounce back to
 * where it started: search the way the jump was heading first, then the
 * other way as a fallback. Leaves the selection put only when every
 * filtered row is disabled. */
static void snap_enabled_dir(struct picker_state *s, int dir)
{
    if (s->n_filtered == 0 || picker_core_row_enabled(s, s->sel))
        return;
    if (dir < 0) {
        for (size_t i = s->sel; i-- > 0;)
            if (picker_core_row_enabled(s, i)) {
                s->sel = i;
                return;
            }
        for (size_t i = s->sel + 1; i < s->n_filtered; i++)
            if (picker_core_row_enabled(s, i)) {
                s->sel = i;
                return;
            }
    } else {
        for (size_t i = s->sel + 1; i < s->n_filtered; i++)
            if (picker_core_row_enabled(s, i)) {
                s->sel = i;
                return;
            }
        for (size_t i = s->sel; i-- > 0;)
            if (picker_core_row_enabled(s, i)) {
                s->sel = i;
                return;
            }
    }
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
    snap_enabled_dir(s, dir);
    center_on_sel(s);
}

void picker_core_select_first(struct picker_state *s)
{
    s->sel = 0;
    clamp_scroll(s);
    ensure_enabled_sel(s); /* first enabled at/after the top */
}

void picker_core_select_last(struct picker_state *s)
{
    s->sel = s->n_filtered ? s->n_filtered - 1 : 0;
    clamp_scroll(s);
    ensure_enabled_sel(s); /* last enabled at/before the bottom */
}
