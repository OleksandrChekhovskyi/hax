/* SPDX-License-Identifier: MIT */
#include "terminal/vt_resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "text/utf8.h"

/* One run of bytes in the current row. `width` is the run's cell count:
 * 0 marks a zero-width run (SGR, any passed-through escape), which
 * occupies no column but must survive in place so styling and terminal
 * state stay attached to the text they wrapped. Consecutive glyphs
 * appended at the end of a row coalesce into one run, so an ordinary row
 * costs a couple of allocations rather than one per character. */
struct seg {
    char *b;
    size_t len;
    int width;
};

struct row {
    struct seg *v;
    size_t n, cap;
    size_t cur; /* cursor column, 0..cols */
};

/* Ceiling on a cursor column, and on a CSI numeric parameter before it is
 * clamped to that. A cursor parked past the end of the row materializes as
 * padding when the next glyph lands, so an unbounded column is unbounded
 * work — and these bytes are not all ours: assistant text reaches the
 * terminal (and this transform) with escapes intact, so `ESC[2147483647C`
 * is a thing a model can say. Wider than any real terminal, so nothing we
 * emit ourselves is affected. */
#define VT_MAX_COL   4096
#define VT_PARAM_MAX 1000000

static void row_free(struct row *r)
{
    for (size_t i = 0; i < r->n; i++)
        free(r->v[i].b);
    free(r->v);
    r->v = NULL;
    r->n = r->cap = 0;
    r->cur = 0;
}

static void row_reset(struct row *r)
{
    for (size_t i = 0; i < r->n; i++)
        free(r->v[i].b);
    r->n = 0;
    r->cur = 0;
}

static struct seg *row_insert(struct row *r, size_t at, const char *b, size_t len, int width)
{
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 16;
        r->v = xrealloc(r->v, r->cap * sizeof(*r->v));
    }
    if (at < r->n)
        memmove(&r->v[at + 1], &r->v[at], (r->n - at) * sizeof(*r->v));
    r->v[at].b = xmalloc(len);
    memcpy(r->v[at].b, b, len);
    r->v[at].len = len;
    r->v[at].width = width;
    r->n++;
    return &r->v[at];
}

static size_t row_cols(const struct row *r)
{
    size_t cols = 0;
    for (size_t i = 0; i < r->n; i++)
        cols += (size_t)r->v[i].width;
    return cols;
}

/* Byte offset inside `s` of the cell boundary at or before column `col`,
 * counted from the run's start, for splitting a coalesced glyph run.
 * *cols_before receives the columns actually skipped, which is less than
 * `col` when `col` lands inside a double-width glyph: the cut goes *before*
 * that glyph, never through it. A cursor addressed into the second half of
 * a wide cell therefore owns the whole cell, which is what a terminal does
 * — half a glyph is not something it can show. */
static size_t byte_at_col(const char *s, size_t len, size_t col, size_t *cols_before)
{
    size_t i = 0, c = 0;
    while (i < len && c < col) {
        size_t consumed = 1;
        int w = utf8_codepoint_cells(s, len, i, &consumed);
        if (w < 0)
            w = 1;
        if (c + (size_t)w > col)
            break; /* col lands inside this glyph */
        c += (size_t)w;
        i += consumed ? consumed : 1;
    }
    *cols_before = c;
    return i;
}

/* Index of the run holding column `col`, splitting a coalesced run when the
 * column falls inside it. Zero-width runs sitting at that column stay to
 * the left of the returned index, so an SGR run written at the cursor lands
 * before the glyph it styles. Returns r->n when `col` is at or past the end
 * of the row. */
static size_t row_split(struct row *r, size_t col)
{
    size_t c = 0;
    for (size_t i = 0; i < r->n; i++) {
        if (r->v[i].width == 0)
            continue;
        if (c == col)
            return i;
        if (c + (size_t)r->v[i].width > col) {
            /* Split run i at the cell boundary at or before `col`. */
            struct seg *s = &r->v[i];
            size_t head_cols = 0;
            size_t off = byte_at_col(s->b, s->len, col - c, &head_cols);
            if (off == 0)
                return i; /* col lands in this run's first cell */
            size_t tail_len = s->len - off;
            int head_w = (int)head_cols;
            int tail_w = s->width - head_w;
            char *tail = xmalloc(tail_len ? tail_len : 1);
            memcpy(tail, s->b + off, tail_len);
            s->len = off;
            s->width = head_w;
            row_insert(r, i + 1, tail, tail_len, tail_w);
            free(tail);
            return i + 1;
        }
        c += (size_t)r->v[i].width;
    }
    return r->n;
}

/* Write a glyph of `width` cells at the cursor. Past the end of the row
 * it appends (coalescing onto a trailing glyph run); inside the row it
 * replaces the columns it covers, which is how the tool block's closing
 * "\r └" overprint lands on the leading strip glyph without disturbing
 * the rest of the row. */
static void row_put(struct row *r, const char *b, size_t len, int width)
{
    size_t cols = row_cols(r);
    if (r->cur >= cols) {
        /* Pad a cursor parked past the end (CSI nC) with spaces — as one
         * run, not one per cell: the column is clamped (VT_MAX_COL) but
         * still far larger than anything we emit, and a run of spaces
         * splits and erases exactly like a coalesced glyph run would. */
        if (r->cur > cols) {
            size_t pad = r->cur - cols;
            char *sp = xmalloc(pad);
            memset(sp, ' ', pad);
            row_insert(r, r->n, sp, pad, (int)pad);
            free(sp);
        }
        if (r->n > 0 && r->v[r->n - 1].width > 0) {
            struct seg *s = &r->v[r->n - 1];
            s->b = xrealloc(s->b, s->len + len);
            memcpy(s->b + s->len, b, len);
            s->len += len;
            s->width += width;
        } else {
            row_insert(r, r->n, b, len, width);
        }
        r->cur += (size_t)width;
        return;
    }
    size_t start = row_split(r, r->cur);
    size_t end = row_split(r, r->cur + (size_t)width);
    /* Drop the covered glyph runs, keeping any zero-width run among them
     * so styling that was already in effect survives the overwrite. */
    size_t w = start;
    for (size_t i = start; i < end; i++) {
        if (r->v[i].width == 0)
            r->v[w++] = r->v[i];
        else
            free(r->v[i].b);
    }
    if (w != end) {
        memmove(&r->v[w], &r->v[end], (r->n - end) * sizeof(*r->v));
        r->n -= end - w;
    }
    row_insert(r, start, b, len, width);
    r->cur += (size_t)width;
}

/* Zero-width escape run (SGR, unmodeled sequence): insert at the cursor so
 * it keeps its position relative to the glyphs around it. */
static void row_put_raw(struct row *r, const char *b, size_t len)
{
    size_t at = (r->cur >= row_cols(r)) ? r->n : row_split(r, r->cur);
    row_insert(r, at, b, len, 0);
}

/* Zero-width *content* — a combining mark — rides the glyph it follows, so
 * it is appended to that glyph's run rather than stored as its own
 * zero-width run. That keeps it inseparable from its base cell: an erase or
 * overwrite that takes the base must take the mark with it, or the row
 * would end up carrying an orphan accent. (Zero-width escapes are the
 * opposite case — they are terminal state and deliberately survive an
 * erase, which is why the two go in through different paths.) */
static void row_put_combining(struct row *r, const char *b, size_t len)
{
    if (r->cur >= row_cols(r) && r->n > 0 && r->v[r->n - 1].width > 0) {
        struct seg *s = &r->v[r->n - 1];
        s->b = xrealloc(s->b, s->len + len);
        memcpy(s->b + s->len, b, len);
        s->len += len;
        return;
    }
    /* No glyph to ride (row start, or the cursor was moved back into the
     * row): keep it in place as its own run. */
    row_put_raw(r, b, len);
}

/* Erase cells: CSI 0K (the only form hax emits) from the cursor to the end
 * of the row, CSI 1K from the row start through the cursor's own cell, CSI
 * 2K the whole row. None of the three move the cursor — a glyph written
 * afterwards lands at the same column it would have. Zero-width runs are
 * retained (appended after the surviving glyphs): on a real terminal an
 * erase clears cells but not the pending SGR state, and the markdown
 * wrapper's retro-wrap relies on that — it re-emits only the style deltas
 * it thinks are missing after erasing.
 *
 * 0K and 2K clear everything from `from` to the row's end, so dropping
 * those runs is enough: the next write pads back out to the cursor. 1K can
 * leave content to its right, which has to keep its columns, so its span
 * becomes spaces instead. */
static void row_erase(struct row *r, int param)
{
    size_t cols = row_cols(r);
    size_t from = (param == 1 || param == 2) ? 0 : r->cur;
    size_t to = param == 1 ? r->cur + 1 : cols;
    if (to > cols)
        to = cols;
    if (from >= to)
        return;

    size_t start = row_split(r, from);
    size_t end = row_split(r, to);
    size_t w = start;
    for (size_t i = start; i < end; i++) {
        if (r->v[i].width == 0)
            r->v[w++] = r->v[i];
        else
            free(r->v[i].b);
    }
    if (w != end) {
        memmove(&r->v[w], &r->v[end], (r->n - end) * sizeof(*r->v));
        r->n -= end - w;
    }
    if (param != 1)
        return;
    /* Blank the cleared span only when a glyph survives to its right; at the
     * row's end there is nothing to hold in place and spaces would just be
     * trailing whitespace the terminal doesn't show either. */
    for (size_t i = w; i < r->n; i++) {
        if (r->v[i].width > 0) {
            size_t pad = to - from;
            char *sp = xmalloc(pad);
            memset(sp, ' ', pad);
            row_insert(r, w, sp, pad, (int)pad);
            free(sp);
            return;
        }
    }
}

static void row_commit(struct row *r, FILE *out)
{
    for (size_t i = 0; i < r->n; i++)
        fwrite(r->v[i].b, 1, r->v[i].len, out);
    fputc('\n', out);
    row_reset(r);
}

/* Byte length of the escape sequence starting at bytes[i] (which is
 * ESC), and its CSI final byte + first numeric parameter when it is a
 * CSI. `final` is 0 for anything that isn't a CSI. Unterminated
 * sequences consume the rest of the buffer. */
static size_t esc_len(const char *bytes, size_t n, size_t i, char *final, int *param,
                      int *has_param)
{
    *final = 0;
    *param = 0;
    *has_param = 0;
    size_t j = i + 1;
    if (j >= n)
        return n - i;
    if (bytes[j] == '[') {
        j++;
        int val = 0, seen = 0;
        while (j < n) {
            unsigned char c = (unsigned char)bytes[j];
            if (c >= '0' && c <= '9') {
                /* Saturate rather than overflow: the bytes can carry a
                 * model's raw escape (nothing strips ESC out of assistant
                 * text), and `val * 10` on a long digit run is undefined
                 * behavior. Any value this large is clamped by the caller
                 * anyway. */
                if (val < VT_PARAM_MAX)
                    val = val * 10 + (c - '0');
                seen = 1;
                j++;
                continue;
            }
            if (c == ';' || c == '?' || c == ':') {
                /* Only the first parameter is ever consulted. */
                j++;
                continue;
            }
            if (c >= 0x40 && c <= 0x7e) {
                *final = (char)c;
                *param = val;
                *has_param = seen;
                return j + 1 - i;
            }
            j++;
        }
        return n - i;
    }
    if (bytes[j] == ']') {
        /* OSC: terminated by BEL or ST (ESC \). */
        j++;
        while (j < n) {
            if (bytes[j] == '\a')
                return j + 1 - i;
            if (bytes[j] == 0x1b && j + 1 < n && bytes[j + 1] == '\\')
                return j + 2 - i;
            j++;
        }
        return n - i;
    }
    return 2; /* ESC + one byte */
}

void vt_resolve(const char *bytes, size_t n, FILE *out)
{
    struct row r = {0};
    size_t i = 0;
    while (i < n) {
        char c = bytes[i];
        if (c == '\n') {
            row_commit(&r, out);
            i++;
            continue;
        }
        if (c == '\r') {
            r.cur = 0;
            i++;
            continue;
        }
        if (c == 0x1b) {
            char final;
            int param, has_param;
            size_t len = esc_len(bytes, n, i, &final, &param, &has_param);
            switch (final) {
            case 'D': {
                size_t back = has_param && param > 0 ? (size_t)param : 1;
                r.cur = r.cur > back ? r.cur - back : 0;
                break;
            }
            case 'C':
                r.cur += has_param && param > 0 ? (size_t)param : 1;
                if (r.cur > VT_MAX_COL)
                    r.cur = VT_MAX_COL;
                break;
            case 'K':
                row_erase(&r, has_param ? param : 0);
                break;
            default:
                row_put_raw(&r, bytes + i, len);
                break;
            }
            i += len;
            continue;
        }
        size_t consumed = 1;
        int w = utf8_codepoint_cells(bytes, n, i, &consumed);
        if (consumed == 0)
            consumed = 1;
        if (w < 0)
            w = 1;
        if (w == 0)
            row_put_combining(&r, bytes + i, consumed);
        else
            row_put(&r, bytes + i, consumed, w);
        i += consumed;
    }
    /* A trailing partial row still holds content the terminal would be
     * showing; terminate it so the sink ends on a whole line. */
    if (r.n > 0)
        row_commit(&r, out);
    row_free(&r);
}
