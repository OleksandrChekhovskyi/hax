/* SPDX-License-Identifier: MIT */
#ifndef HAX_MARKDOWN_WRAP_H
#define HAX_MARKDOWN_WRAP_H

#include <stddef.h>

#include "util.h"
#include "text/utf8.h"

/* Private eager terminal wrapper for Markdown output. Visible bytes are emitted
 * immediately while the current row is shadowed for retroactive word wrapping.
 * The parser supplies semantic style state and selects wrapped or verbatim output. */
struct md_wrap {
    /* row_meta tags content (0) and raw bytes (1); cp_stream provides cells. */
    int width;
    struct utf8_stream cp_stream;
    struct buf row_buf;
    struct buf row_meta;
    int col; /* visible cells; raw escapes are zero-width */

    /* Most recent break-space: its row_buf offset and post-space cell column. */
    int last_break_byte;
    int last_break_col;

    /* Continuation indent is detected once, before replay drops the original
     * line prefix, then locked until a hard newline. */
    int indent_cells;
    int indent_locked;
    /* Leading spaces are not break candidates until content has arrived. */
    int row_has_content;

    /* An edge-space cannot be emitted safely with delayed terminal autowrap.
     * Defer its newline until visible content commits it; a hard newline
     * absorbs it. */
    int pending_wrap;

    /* Style at the break-space. Later eager SGRs may have changed terminal
     * state before a retroactive break erases and replays the partial word. */
    int snap_in_bold;
    int snap_in_italic;
    int snap_in_inline_code;
};

/* Refreshed before each operation so retro-wrap can restore semantic styles. */
struct md_wrap_context {
    void (*emit)(const char *bytes, size_t n, int is_raw, void *user);
    void *user;
    int styled;
    int in_bold;
    int in_italic;
    int in_inline_code;
};

/* Reset for a new stream while retaining allocated buffers. */
void md_wrap_reset(struct md_wrap *w, int width);
void md_wrap_free(struct md_wrap *w);
int md_wrap_width(const struct md_wrap *w);

/* Discard the row shadow after a caller emits complete lines directly. */
void md_wrap_row_reset(struct md_wrap *w);

/* Emit visible content with UTF-8 cell accounting and tab normalization.
 * Verbatim output, used for headings and fences, bypasses wrapping. */
void md_wrap_emit_text(struct md_wrap *w, const struct md_wrap_context *ctx, const char *s,
                       size_t n, int verbatim);
/* Emit a counted zero-width raw run, retaining it only when replay may need it. */
void md_wrap_emit_raw(struct md_wrap *w, const struct md_wrap_context *ctx, const char *s, size_t n,
                      int verbatim);

/* Resolve an edge wrap before direct block output; a hard newline resolves it itself. */
void md_wrap_commit_pending(struct md_wrap *w, const struct md_wrap_context *ctx);
/* Drain partial UTF-8 and discard the emitted row shadow without adding a newline. */
void md_wrap_flush(struct md_wrap *w, const struct md_wrap_context *ctx);

#endif /* HAX_MARKDOWN_WRAP_H */
