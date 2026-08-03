/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_PICKER_H
#define HAX_TERMINAL_PICKER_H

#include <stddef.h>

struct picker_item {
    const char *label;  /* filter target and primary row text */
    const char *detail; /* optional terse metadata shown on the same row */
    int dim;            /* advisory only; dim rows remain selectable */
    int current;
    /* Optional theme foreground opening sequence. Ignored when `dim` is set. */
    const char *label_color;
    /* Optional explanatory text shown below the list for the selected item. */
    const char *description;
};

struct picker_opts {
    const char *title;
    const struct picker_item *items; /* required when `item_count` is nonzero */
    size_t item_count;
    const char *empty_message; /* optional note shown for an empty list */
    size_t initial_index;      /* defaults to 0 */
    /* Show a clipped label in full below the list. */
    int repeat_clipped_label;
};

/* Opens an interactive, type-to-filter list and returns an index into `items`. The filter matches
 * every space-separated query term as an ASCII-case-insensitive substring of `label`. Returns -1
 * on cancellation, an empty list, or non-TTY input/output. All pointers are borrowed for the
 * duration of the call. */
long picker_run(const struct picker_opts *options);

#endif /* HAX_TERMINAL_PICKER_H */
