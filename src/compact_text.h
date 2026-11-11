/* SPDX-License-Identifier: MIT */
#ifndef HAX_COMPACT_TEXT_H
#define HAX_COMPACT_TEXT_H

#include <stddef.h>

struct item;

/* The model-visible window as labeled plain text, for a summary request that no longer fits as
 * native conversation. Reasoning and usage records are left out and images become markers. */

struct compact_text {
    char *text;  /* owned */
    long tokens; /* text_token_estimate() of `text` */
};

/* Render items[0, n_items) within `max_tokens` estimated tokens (LONG_MAX for no limit),
 * shortening the oldest turns first and marking each removal in place. A previous seed and the
 * latest typed prompt stay whole, and the newest turn is never left out. Returns 0, or -1 with
 * `out` zeroed when the text cannot be shortened that far. */
int compact_text_fit(const struct item *items, size_t n_items, long max_tokens,
                     struct compact_text *out);

void compact_text_destroy(struct compact_text *text);

#endif /* HAX_COMPACT_TEXT_H */
