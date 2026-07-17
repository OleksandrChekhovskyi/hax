/* SPDX-License-Identifier: MIT */
#ifndef HAX_MARKDOWN_TABLE_H
#define HAX_MARKDOWN_TABLE_H

#include <stddef.h>

#include "util.h"

/* Private GFM table collector and terminal renderer. Tables buffer every row because column sizing
 * and the aligned-versus-reflowed decision require the complete block. The parser supplies inline
 * cell rendering and wrapped output through md_table_context. */
struct md_table {
    int collecting;
    struct buf buf;
};

/* ADVANCED consumes through the updated offset; DEFER and PASS leave it unconsumed. */
enum md_table_result {
    MD_TABLE_ADVANCED,
    MD_TABLE_DEFER,
    MD_TABLE_PASS,
};

typedef void (*md_table_emit_fn)(const char *bytes, size_t n, int is_raw, void *user);

/* Parser-owned rendering operations, kept narrow so md_renderer remains private. */
struct md_table_context {
    void *user;

    /* Aligned grids bypass wrapping; reflow and lossless fallback use wrapped output. */
    md_table_emit_fn emit_direct;
    void (*emit_text)(void *user, const char *s, size_t n);
    void (*emit_raw)(void *user, const char *s, size_t n);

    /* Reflowed styles update parser state so wrap continuations can restore active SGR. */
    void (*replay_raw)(void *user, const char *s, size_t n);
    void (*open_bold)(void *user);
    void (*close_bold)(void *user);

    /* Render one inline-only cell; bold_base suppresses toggles inside an already-bold label. */
    void (*render_inline)(void *user, const char *s, size_t n, int bold_base, md_table_emit_fn emit,
                          void *emit_user);

    /* Resolve an eager edge wrap before layout and discard the completed row shadow afterward. */
    void (*commit_pending)(void *user);
    void (*row_reset)(void *user);

    int styled;     /* gate direct SGR output */
    int wrap_width; /* <= 0 means unlimited */
};

/* Reset collection while retaining the buffer allocation. */
void md_table_reset(struct md_table *t);
void md_table_free(struct md_table *t);
/* True while rows are being buffered without output. */
int md_table_is_collecting(const struct md_table *t);

/* Probe a complete header/delimiter pair and begin collection on a match. */
enum md_table_result md_table_try_start(struct md_table *t, const struct buf *work, size_t *offset);
/* Consume one complete body row, or finalize before a terminating line and return PASS. */
enum md_table_result md_table_step(struct md_table *t, const struct md_table_context *ctx,
                                   const struct buf *work, size_t *offset);

/* Bail an oversized newline-less row and the buffered block to wrapped, lossless output. */
int md_table_bail_partial(struct md_table *t, const struct md_table_context *ctx,
                          const char *partial, size_t len);

/* Resolve a deferred header or final row, then render any table still collecting at EOF. */
void md_table_finish(struct md_table *t, const struct md_table_context *ctx, struct buf *tail);

#endif /* HAX_MARKDOWN_TABLE_H */
