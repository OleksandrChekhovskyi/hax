/* SPDX-License-Identifier: MIT */
#ifndef HAX_MARKDOWN_H
#define HAX_MARKDOWN_H

#include <stddef.h>

/* Pragmatic streaming Markdown renderer, not a CommonMark implementation. Supports emphasis,
 * inline and fenced code, line-start headings and lists, thematic breaks, and leading-pipe GFM
 * tables. Wide tables reflow to bulleted label/value records. Prose newlines normally become
 * spaces; two trailing spaces or a block boundary preserve a hard break.
 *
 * Parser and style state span feeds, with ambiguous suffixes deferred until more input arrives.
 * Call md_flush at end of turn and md_reset before the next turn. */

/* is_raw distinguishes zero-width ANSI escapes from visible content. Consumers must not let raw
 * bytes alter held-newline or other display-width bookkeeping. */
typedef void (*md_emit_fn)(const char *bytes, size_t n, int is_raw, void *user);

struct md_renderer;

/* Create a renderer for the given display width. <= 0 disables wrapping and table reflow.
 * Headings remain unwrapped; fenced code remains verbatim. */
struct md_renderer *md_new(md_emit_fn emit, void *user, int wrap_width);

/* Reset for a new turn, discarding pending state, restoring defaults, and applying the width. */
void md_reset(struct md_renderer *m, int wrap_width);

/* Release the renderer without flushing pending output. Accepts NULL. */
void md_free(struct md_renderer *m);

/* Consume one input chunk, deferring ambiguous suffixes until a later feed or flush. */
void md_feed(struct md_renderer *m, const char *s, size_t n);

/* Finish the turn by resolving deferred input, closing styles, and emitting buffered output. */
void md_flush(struct md_renderer *m);

/* Enable or suppress SGR output without disabling wrapping or block parsing. A mode change flushes
 * deferred input under the old setting, then resets parser and wrap state; the caller owns any
 * ANSI_RESET at the seam. Styling is enabled by default. */
void md_set_styled(struct md_renderer *m, int on);

/* True while a GFM table is buffered for whole-table layout and no rows are emitted. */
int md_in_table(const struct md_renderer *m);

#endif /* HAX_MARKDOWN_H */
