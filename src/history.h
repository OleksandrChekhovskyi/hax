/* SPDX-License-Identifier: MIT */
#ifndef HAX_HISTORY_H
#define HAX_HISTORY_H

#include <stddef.h>

#include "render/render_ctx.h"

struct item;

enum history_detail {
    /* Collapse every tool call to a breadcrumb and omit its result. */
    HISTORY_BRIEF,
    /* Rebuild verbose previews; calls configured as collapsed remain breadcrumbs. */
    HISTORY_FULL,
};

/* Render items[start_idx, n_items) in their user-facing form. The caller owns surrounding block
 * separation and final state normalization. show_reasoning controls reasoning visibility; a NULL
 * markdown renderer emits unwrapped text. */
void history_render(struct render_ctx *render, enum history_detail detail, const struct item *items,
                    size_t n_items, size_t start_idx);

#endif /* HAX_HISTORY_H */
