/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_PICKER_CORE_H
#define HAX_TERMINAL_PICKER_CORE_H

#include <stddef.h>

#include "buf.h"
#include "terminal/picker.h"

#define PICKER_MARKER_CELLS               2
#define PICKER_CURRENT_TAG_CELLS          11
#define PICKER_DETAIL_SEPARATOR_CELLS     2
#define PICKER_DIM_DETAIL_SEPARATOR_CELLS 3

enum picker_direction {
    PICKER_DIRECTION_PREVIOUS = -1,
    PICKER_DIRECTION_NEXT = 1,
};

struct picker_core {
    const struct picker_opts *options; /* borrowed */
    size_t *matches;                   /* item indices in filter order; caller-owned storage */
    size_t match_count;
    size_t selection;     /* index into `matches` */
    size_t first_visible; /* index into `matches` */
    int viewport_rows;
    struct buf query;
};

/* Visible cells before the first line break, including an ellipsis when text follows it. */
int picker_core_text_cells(const char *text);

/* Number of cells allocated to an item's label within a terminal row. */
int picker_core_label_cells(const struct picker_item *item, int terminal_cols);

/* Appends text while replacing terminal control and unsafe Unicode codepoints with '?'. */
void picker_core_append_sanitized(struct buf *output, const char *text, size_t len);

/* Every space-separated query term must occur in `text`; ASCII case is ignored. An empty or NULL
 * query matches any text, while NULL text is treated as empty. */
int picker_core_match(const char *text, const char *query);

/* Rebuilds `matches` for the current query; storage must hold `options->item_count` entries. */
void picker_core_update_matches(struct picker_core *core);

/* Restores selection and scroll invariants after a viewport change. */
void picker_core_clamp_view(struct picker_core *core);

void picker_core_move_selection(struct picker_core *core, enum picker_direction direction);
void picker_core_page_selection(struct picker_core *core, enum picker_direction direction);
void picker_core_select_first(struct picker_core *core);
void picker_core_select_last(struct picker_core *core);

/* Selects and centers `item_index` if it is present in `matches`. */
void picker_core_select_item(struct picker_core *core, size_t item_index);

#endif /* HAX_TERMINAL_PICKER_CORE_H */
