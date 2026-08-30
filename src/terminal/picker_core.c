/* SPDX-License-Identifier: MIT */
#include "terminal/picker_core.h"

#include <string.h>

#include "buf.h"
#include "terminal/picker.h"
#include "text/utf8.h"

int picker_core_text_cells(const char *text)
{
    size_t len = strlen(text);
    size_t line_len = strcspn(text, "\r\n");
    int cells = 0;

    for (size_t offset = 0; offset < line_len;) {
        size_t bytes;
        int width = utf8_codepoint_cells(text, line_len, offset, &bytes);
        cells += width < 0 ? 1 : width;
        offset += bytes ? bytes : 1;
    }
    return cells + (line_len < len ? 1 : 0);
}

int picker_core_label_cells(const struct picker_item *item, int terminal_cols)
{
    int row_cells = terminal_cols - PICKER_MARKER_CELLS;
    if (row_cells < 1)
        row_cells = 1;

    int current_cells = item->current ? PICKER_CURRENT_TAG_CELLS : 0;
    int separator_cells =
        item->dim ? PICKER_DIM_DETAIL_SEPARATOR_CELLS : PICKER_DETAIL_SEPARATOR_CELLS;
    int available_cells = row_cells - current_cells;
    int label_cells = available_cells;

    if (item->detail && item->detail[0]) {
        const char *label = item->label ? item->label : "";
        int natural_cells = picker_core_text_cells(label);
        int detail_cells = picker_core_text_cells(item->detail);
        int detail_adjusted_cells = available_cells - separator_cells - detail_cells;
        int minimum_cells = available_cells / 2;

        label_cells = minimum_cells > detail_adjusted_cells ? minimum_cells : detail_adjusted_cells;
        if (natural_cells < label_cells)
            label_cells = natural_cells;
    }
    return label_cells < 1 ? 1 : label_cells;
}

void picker_core_append_sanitized(struct buf *output, const char *text, size_t len)
{
    for (size_t offset = 0; offset < len;) {
        size_t bytes;
        int width = utf8_codepoint_cells(text, len, offset, &bytes);
        if (width < 0)
            buf_append(output, "?", 1);
        else
            buf_append(output, text + offset, bytes ? bytes : 1);
        offset += bytes ? bytes : 1;
    }
}

static unsigned char ascii_lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static int contains_ascii_case_insensitive(const char *text, const char *term, size_t term_len)
{
    if (term_len == 0)
        return 1;

    for (const char *candidate = text; *candidate; candidate++) {
        size_t i = 0;
        while (i < term_len && candidate[i] &&
               ascii_lower((unsigned char)candidate[i]) == ascii_lower((unsigned char)term[i]))
            i++;
        if (i == term_len)
            return 1;
    }
    return 0;
}

int picker_core_match(const char *text, const char *query)
{
    if (!query || !query[0])
        return 1;
    if (!text)
        text = "";

    const char *term = query;
    while (*term) {
        while (*term == ' ')
            term++;
        const char *end = term;
        while (*end && *end != ' ')
            end++;
        if (end != term && !contains_ascii_case_insensitive(text, term, (size_t)(end - term)))
            return 0;
        term = end;
    }
    return 1;
}

static size_t viewport_rows(const struct picker_core *core)
{
    return core->viewport_rows > 0 ? (size_t)core->viewport_rows : 1;
}

void picker_core_clamp_view(struct picker_core *core)
{
    if (core->match_count == 0) {
        core->selection = 0;
        core->first_visible = 0;
        return;
    }

    size_t visible_rows = viewport_rows(core);
    if (core->selection >= core->match_count)
        core->selection = core->match_count - 1;
    if (core->selection < core->first_visible)
        core->first_visible = core->selection;
    else if (core->selection >= core->first_visible + visible_rows)
        core->first_visible = core->selection - visible_rows + 1;

    if (core->match_count <= visible_rows)
        core->first_visible = 0;
    else if (core->first_visible + visible_rows > core->match_count)
        core->first_visible = core->match_count - visible_rows;
}

void picker_core_update_matches(struct picker_core *core)
{
    const char *query = core->query.len ? core->query.data : "";
    core->match_count = 0;
    for (size_t i = 0; i < core->options->item_count; i++) {
        if (picker_core_match(core->options->items[i].label, query))
            core->matches[core->match_count++] = i;
    }

    core->selection = 0;
    core->first_visible = 0;
}

void picker_core_move_selection(struct picker_core *core, enum picker_direction direction)
{
    if (core->match_count == 0)
        return;
    if (direction == PICKER_DIRECTION_PREVIOUS) {
        if (core->selection > 0)
            core->selection--;
    } else if (direction == PICKER_DIRECTION_NEXT && core->selection + 1 < core->match_count) {
        core->selection++;
    }
    picker_core_clamp_view(core);
}

static void center_selection(struct picker_core *core)
{
    size_t visible_rows = viewport_rows(core);
    if (core->match_count <= visible_rows) {
        core->first_visible = 0;
        return;
    }

    size_t half_viewport = visible_rows / 2;
    core->first_visible = core->selection > half_viewport ? core->selection - half_viewport : 0;
    if (core->first_visible + visible_rows > core->match_count)
        core->first_visible = core->match_count - visible_rows;
}

void picker_core_page_selection(struct picker_core *core, enum picker_direction direction)
{
    if (core->match_count == 0)
        return;

    size_t step = viewport_rows(core) / 2;
    if (step < 1)
        step = 1;
    if (direction == PICKER_DIRECTION_PREVIOUS) {
        core->selection = core->selection > step ? core->selection - step : 0;
    } else if (direction == PICKER_DIRECTION_NEXT) {
        core->selection = core->selection + step < core->match_count ? core->selection + step
                                                                     : core->match_count - 1;
    }
    center_selection(core);
}

void picker_core_select_first(struct picker_core *core)
{
    core->selection = 0;
    picker_core_clamp_view(core);
}

void picker_core_select_last(struct picker_core *core)
{
    core->selection = core->match_count ? core->match_count - 1 : 0;
    picker_core_clamp_view(core);
}

void picker_core_select_item(struct picker_core *core, size_t item_index)
{
    for (size_t match = 0; match < core->match_count; match++) {
        if (core->matches[match] == item_index) {
            core->selection = match;
            center_selection(core);
            return;
        }
    }
}
