/* SPDX-License-Identifier: MIT */
#include "render/markdown_wrap.h"

#include <stdio.h>
#include <string.h>

#include "terminal/ansi.h"
#include "terminal/theme.h"

/* Content cells granted past the indent only on a doomed continuation row
 * (indent_cells >= wrap_width). */
#define WRAP_MIN_CONTENT 8

void md_wrap_row_reset(struct md_wrap *w)
{
    buf_reset(&w->row_buf);
    buf_reset(&w->row_meta);
    w->col = 0;
    w->last_break_byte = -1;
    w->last_break_col = 0;
    w->indent_cells = 0;
    w->indent_locked = 0;
    w->row_has_content = 0;
}

void md_wrap_reset(struct md_wrap *w, int width)
{
    w->width = width;
    utf8_stream_reset(&w->cp_stream);
    md_wrap_row_reset(w);
    w->pending_wrap = 0;
    w->snap_in_bold = 0;
    w->snap_in_italic = 0;
    w->snap_in_inline_code = 0;
}

void md_wrap_free(struct md_wrap *w)
{
    buf_free(&w->row_buf);
    buf_free(&w->row_meta);
}

static void wrap_emit_indent(const struct md_wrap_context *ctx, int n);

/* Materialize a deferred edge wrap before visible or direct block output.
 * A hard newline absorbs it instead. */
void md_wrap_commit_pending(struct md_wrap *w, const struct md_wrap_context *ctx)
{
    if (!w->pending_wrap)
        return;
    w->pending_wrap = 0;
    ctx->emit("\n", 1, 0, ctx->user);
    wrap_emit_indent(ctx, w->indent_cells);
    buf_reset(&w->row_buf);
    buf_reset(&w->row_meta);
    w->col = w->indent_cells;
    w->last_break_byte = -1;
    w->last_break_col = 0;
    w->row_has_content = 0;
    w->snap_in_bold = 0;
    w->snap_in_italic = 0;
    w->snap_in_inline_code = 0;
}

/* Emit eagerly while shadowing the row and byte kinds for retroactive replay.
 * Raw bytes do not commit a pending edge wrap: only visible content should open
 * the next row, and a following hard newline may still absorb the wrap. */
static void wrap_append(struct md_wrap *w, const struct md_wrap_context *ctx, const char *s,
                        size_t n, int is_raw)
{
    if (is_raw && w->pending_wrap) {
        ctx->emit(s, n, 1, ctx->user);
        return;
    }
    md_wrap_commit_pending(w, ctx);
    buf_append(&w->row_buf, s, n);
    char meta = is_raw ? 1 : 0;
    for (size_t i = 0; i < n; i++)
        buf_append(&w->row_meta, &meta, 1);
    ctx->emit(s, n, is_raw, ctx->user);
}

/* Replay runs of like byte kind so consumers can ignore zero-width raw bytes. */
static void wrap_flush_range(struct md_wrap *w, const struct md_wrap_context *ctx, size_t start,
                             size_t end)
{
    size_t i = start;
    while (i < end) {
        char kind = w->row_meta.data[i];
        size_t j = i + 1;
        while (j < end && w->row_meta.data[j] == kind)
            j++;
        ctx->emit(w->row_buf.data + i, j - i, kind ? 1 : 0, ctx->user);
        i = j;
    }
}

/* The shadow is already emitted. Keep pending_wrap for the next byte or break. */
static void wrap_flush_all(struct md_wrap *w)
{
    buf_reset(&w->row_buf);
    buf_reset(&w->row_meta);
}

/* Derive continuation indent from leading spaces and an optional list marker.
 * SGR runs may surround the marker; leading spaces are capped at eight cells. */
static int compute_indent_cells(const struct md_wrap *w)
{
    size_t i = 0;
    /* Skip a style opener before leading spaces. */
    while (i < w->row_buf.len && w->row_meta.data[i] == 1)
        i++;
    /* Leading ASCII spaces are preserved as continuation indent. */
    size_t lead_spaces = 0;
    while (i < w->row_buf.len && lead_spaces < 8 && w->row_meta.data[i] == 0 &&
           w->row_buf.data[i] == ' ') {
        i++;
        lead_spaces++;
    }
    /* A dim marker has another style opener after its leading spaces. */
    while (i < w->row_buf.len && w->row_meta.data[i] == 1)
        i++;
    if (i >= w->row_buf.len)
        return 0;
    /* Table reflow uses the one-cell Unicode bullet. */
    if (i + 3 < w->row_buf.len && w->row_meta.data[i] == 0 &&
        (unsigned char)w->row_buf.data[i] == 0xe2 &&
        (unsigned char)w->row_buf.data[i + 1] == 0x80 &&
        (unsigned char)w->row_buf.data[i + 2] == 0xa2 && w->row_meta.data[i + 3] == 0 &&
        w->row_buf.data[i + 3] == ' ')
        return (int)lead_spaces + 2;
    char c = w->row_buf.data[i];
    if ((c == '*' || c == '-' || c == '+') && i + 1 < w->row_buf.len &&
        w->row_meta.data[i + 1] == 0 && w->row_buf.data[i + 1] == ' ')
        return (int)lead_spaces + 2;
    if (c >= '0' && c <= '9') {
        /* Ordered markers may contain the full CommonMark digit run. */
        size_t j = i + 1;
        while (j < w->row_buf.len && w->row_meta.data[j] == 0 && w->row_buf.data[j] >= '0' &&
               w->row_buf.data[j] <= '9')
            j++;
        if (j > i && j + 1 < w->row_buf.len && w->row_meta.data[j] == 0 &&
            (w->row_buf.data[j] == '.' || w->row_buf.data[j] == ')') &&
            w->row_meta.data[j + 1] == 0 && w->row_buf.data[j + 1] == ' ')
            return (int)lead_spaces + (int)(j - i) + 2; /* spaces + digits + delim + space */
    }
    /* Without a marker, retain only the line's leading indent. */
    return (int)lead_spaces;
}

/* The configured width normally avoids the terminal's autowrap edge. If the
 * indent already consumes it, autowrap is unavoidable; allow enough content to
 * prevent continuation rows from advancing one column at a time. */
static int current_row_budget(const struct md_wrap *w)
{
    if (w->indent_cells <= 0 || w->indent_cells < w->width)
        return w->width;
    return w->indent_cells + WRAP_MIN_CONTENT;
}

static void wrap_emit_indent(const struct md_wrap_context *ctx, int n)
{
    if (n <= 0)
        return;
    /* Indent is terminal-only; replay buffers hold content after it. */
    static const char SPACES[] = "                                ";
    while (n > 0) {
        int k = n > 32 ? 32 : n;
        ctx->emit(SPACES, (size_t)k, 0, ctx->user);
        n -= k;
    }
}

/* Erase the eagerly emitted partial word, start a continuation row, and replay
 * it from the shadow buffer without the break-space. */
static void wrap_break(struct md_wrap *w, const struct md_wrap_context *ctx)
{
    if (!w->indent_locked) {
        w->indent_cells = compute_indent_cells(w);
        w->indent_locked = 1;
    }
    /* col is before the new codepoint; last_break_col is after the space. */
    int erase_cells = (w->col - w->last_break_col) + 1;
    if (erase_cells > 0) {
        char esc[16];
        int len = snprintf(esc, sizeof(esc), "\x1b[%dD", erase_cells);
        ctx->emit(esc, (size_t)len, 1, ctx->user);
        ctx->emit(ANSI_ERASE_LINE, sizeof(ANSI_ERASE_LINE) - 1, 1, ctx->user);
    }
    ctx->emit("\n", 1, 0, ctx->user);
    wrap_emit_indent(ctx, w->indent_cells);
    /* Eager post-break SGRs changed terminal state; restore the snapshot before
     * replay. In unstyled mode, emitting a closer would clobber the caller. */
    if (ctx->styled) {
        if (ctx->in_bold != w->snap_in_bold) {
            const char *e = w->snap_in_bold ? ANSI_BOLD : ANSI_BOLD_OFF;
            ctx->emit(e, strlen(e), 1, ctx->user);
        }
        if (ctx->in_italic != w->snap_in_italic) {
            const char *e = w->snap_in_italic ? ANSI_ITALIC : ANSI_ITALIC_OFF;
            ctx->emit(e, strlen(e), 1, ctx->user);
        }
        if (ctx->in_inline_code != w->snap_in_inline_code) {
            const char *e = w->snap_in_inline_code ? theme_open(THEME_CODE_INLINE)
                                                   : theme_close(THEME_CODE_INLINE);
            ctx->emit(e, strlen(e), 1, ctx->user);
        }
    }
    /* Skip the break-space and replay content with interleaved escapes. */
    size_t shift = (size_t)w->last_break_byte + 1;
    if (shift > w->row_buf.len)
        shift = w->row_buf.len;
    if (shift < w->row_buf.len)
        wrap_flush_range(w, ctx, shift, w->row_buf.len);
    size_t new_len = w->row_buf.len - shift;
    memmove(w->row_buf.data, w->row_buf.data + shift, new_len);
    memmove(w->row_meta.data, w->row_meta.data + shift, new_len);
    w->row_buf.len = new_len;
    w->row_meta.len = new_len;
    w->col = w->indent_cells + (w->col - w->last_break_col);
    w->last_break_byte = -1;
    w->last_break_col = 0;
}

/* A hard newline absorbs a pending edge wrap instead of doubling the break. */
static void wrap_hard_newline(struct md_wrap *w, const struct md_wrap_context *ctx)
{
    w->pending_wrap = 0;
    wrap_flush_all(w);
    ctx->emit("\n", 1, 0, ctx->user);
    w->col = 0;
    w->last_break_byte = -1;
    w->last_break_col = 0;
    w->indent_cells = 0;
    w->indent_locked = 0;
    w->row_has_content = 0;
}

/* An overflowing edge-space is dropped and defers its newline. Mid-word
 * overflow erases and replays at the latest prior space. Unbroken words are
 * intentionally left to terminal autowrap. */
static void wrap_consume_codepoint(struct md_wrap *w, const struct md_wrap_context *ctx,
                                   const char *out, size_t n, int cells)
{
    /* CR belongs to CRLF; emitting it could prematurely commit an edge wrap. */
    if (n == 1 && out[0] == '\r')
        return;
    /* Drop spaces straddling a deferred edge wrap; only visible content commits
     * it, while a hard newline absorbs it. */
    if (n == 1 && out[0] == ' ' && w->pending_wrap)
        return;
    md_wrap_commit_pending(w, ctx);
    if (cells == 0) {
        /* Keep combining marks with the prior glyph during replay. */
        wrap_append(w, ctx, out, n, 0);
        return;
    }
    int budget = current_row_budget(w);
    int is_space = (n == 1 && out[0] == ' ');
    /* Leading spaces cannot become edge-wrap candidates. */
    if (is_space && w->row_has_content && w->col + cells > budget) {
        w->pending_wrap = 1;
        return;
    }
    /* Mid-word overflow: wrap at the last break-space. */
    if (!is_space && w->col + cells > budget && w->last_break_byte >= 0)
        wrap_break(w, ctx);
    wrap_append(w, ctx, out, n, 0);
    w->col += cells;
    if (is_space) {
        /* A break before any content would create an empty row without progress. */
        if (w->row_has_content) {
            size_t new_break = w->row_buf.len - 1;
            if (w->last_break_byte < 0 && !w->indent_locked) {
                /* Detect indent before trimming the original line prefix. */
                w->indent_cells = compute_indent_cells(w);
                w->indent_locked = 1;
            }
            /* Retain only the break-space and later bytes needed for replay. */
            size_t shift = new_break;
            if (shift > 0) {
                size_t new_len = w->row_buf.len - shift;
                memmove(w->row_buf.data, w->row_buf.data + shift, new_len);
                memmove(w->row_meta.data, w->row_meta.data + shift, new_len);
                w->row_buf.len = new_len;
                w->row_meta.len = new_len;
                new_break -= shift;
            }
            w->last_break_byte = (int)new_break;
            w->last_break_col = w->col;
            /* Later eager SGRs may require rewinding to this snapshot. */
            w->snap_in_bold = ctx->in_bold;
            w->snap_in_italic = ctx->in_italic;
            w->snap_in_inline_code = ctx->in_inline_code;
        }
    } else {
        w->row_has_content = 1;
    }
}

/* Drain partial UTF-8 before a newline or raw run to preserve byte order. */
static void wrap_drain_cp_stream(struct md_wrap *w, const struct md_wrap_context *ctx)
{
    const char *out;
    size_t out_n;
    int cells;
    if (utf8_stream_flush(&w->cp_stream, &out, &out_n, &cells))
        wrap_consume_codepoint(w, ctx, out, out_n, cells);
}

int md_wrap_width(const struct md_wrap *w)
{
    return w->width;
}

static void emit_text_chunk(struct md_wrap *w, const struct md_wrap_context *ctx, const char *s,
                            size_t n, int verbatim)
{
    if (w->width <= 0 || verbatim) {
        ctx->emit(s, n, 0, ctx->user);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        if (b == '\n') {
            wrap_drain_cp_stream(w, ctx);
            wrap_hard_newline(w, ctx);
            continue;
        }
        const char *out;
        size_t out_n;
        int cells;
        if (utf8_stream_byte(&w->cp_stream, b, &out, &out_n, &cells))
            wrap_consume_codepoint(w, ctx, out, out_n, cells);
    }
}

void md_wrap_emit_text(struct md_wrap *w, const struct md_wrap_context *ctx, const char *s,
                       size_t n, int verbatim)
{
    /* Multiple eager break-spaces can overshoot the budget, so wrapped prose
     * collapses a tab to one space; verbatim output preserves it as four. */
    int bypass = w->width <= 0 || verbatim;
    const char *tab_sub = bypass ? "    " : " ";
    size_t tab_sub_len = bypass ? 4 : 1;
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\t') {
            if (i > start)
                emit_text_chunk(w, ctx, s + start, i - start, verbatim);
            emit_text_chunk(w, ctx, tab_sub, tab_sub_len, verbatim);
            start = i + 1;
        }
    }
    if (start < n)
        emit_text_chunk(w, ctx, s + start, n - start, verbatim);
}

void md_wrap_emit_raw(struct md_wrap *w, const struct md_wrap_context *ctx, const char *s, size_t n,
                      int verbatim)
{
    if (w->width <= 0 || verbatim) {
        if (w->width > 0)
            wrap_drain_cp_stream(w, ctx);
        ctx->emit(s, n, 1, ctx->user);
        return;
    }
    wrap_drain_cp_stream(w, ctx);
    wrap_append(w, ctx, s, n, 1);
}

void md_wrap_flush(struct md_wrap *w, const struct md_wrap_context *ctx)
{
    if (w->width <= 0)
        return;
    wrap_drain_cp_stream(w, ctx);
    wrap_flush_all(w);
}
