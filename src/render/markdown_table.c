/* SPDX-License-Identifier: MIT */
#include "render/markdown_table.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "terminal/ansi.h"
#include "text/utf8.h"

#define TABLE_COL_SEP   3
#define TABLE_MAX_COLS  32
#define TABLE_MAX_ROWS  2048
#define TABLE_MAX_BYTES (1 << 16)

#define GLYPH_BULLET "\xe2\x80\xa2"
#define GLYPH_HLINE  "\xe2\x94\x80"
#define GLYPH_VLINE  "\xe2\x94\x82"
#define GLYPH_CROSS  "\xe2\x94\xbc"

void md_table_reset(struct md_table *t)
{
    t->collecting = 0;
    buf_reset(&t->buf);
}

void md_table_free(struct md_table *t)
{
    buf_free(&t->buf);
}

int md_table_is_collecting(const struct md_table *t)
{
    return t->collecting;
}

static void emit_direct_text(const struct md_table_context *ctx, const char *s, size_t n)
{
    if (n)
        ctx->emit_direct(s, n, 0, ctx->user);
}

static void emit_direct_raw(const struct md_table_context *ctx, const char *s)
{
    if (ctx->styled)
        ctx->emit_direct(s, strlen(s), 1, ctx->user);
}

static void emit_direct_spaces(const struct md_table_context *ctx, int n)
{
    static const char SP[] = "                                ";
    while (n > 0) {
        int k = n > 32 ? 32 : n;
        emit_direct_text(ctx, SP, (size_t)k);
        n -= k;
    }
}

static void emit_direct_glyphs(const struct md_table_context *ctx, int n, const char *g3)
{
    char buf[96];
    while (n > 0) {
        int k = n > 32 ? 32 : n;
        int p = 0;
        for (int x = 0; x < k; x++) {
            buf[p++] = g3[0];
            buf[p++] = g3[1];
            buf[p++] = g3[2];
        }
        emit_direct_text(ctx, buf, (size_t)p);
        n -= k;
    }
}

static void emit_wrapped_text(const struct md_table_context *ctx, const char *s, size_t n)
{
    ctx->emit_text(ctx->user, s, n);
}

static void emit_wrapped_raw(const struct md_table_context *ctx, const char *s, size_t n)
{
    ctx->emit_raw(ctx->user, s, n);
}

static void emit_wrapped_bullet(const struct md_table_context *ctx)
{
    emit_wrapped_raw(ctx, ANSI_DIM, strlen(ANSI_DIM));
    emit_wrapped_text(ctx, GLYPH_BULLET " ", 4);
    emit_wrapped_raw(ctx, ANSI_BOLD_OFF, strlen(ANSI_BOLD_OFF));
}

/* Visible terminal width; non-printing codepoints contribute no cells. */
static int cell_visible_width(const char *s, size_t n)
{
    int w = 0;
    size_t i = 0;
    while (i < n) {
        size_t consumed = 1;
        int c = utf8_codepoint_cells(s, n, i, &consumed);
        if (c > 0)
            w += c;
        i += consumed ? consumed : 1;
    }
    return w;
}

/* Styled bytes plus per-byte raw/content tags; width excludes raw escapes. */
struct cell {
    struct buf bytes;
    struct buf meta;
    int width;
};

/* Preserve callback kinds per byte; width is measured after split UTF-8 calls are assembled. */
static void cell_emit(const char *bytes, size_t n, int is_raw, void *user)
{
    struct cell *c = user;
    buf_append(&c->bytes, bytes, n);
    char meta = is_raw ? 1 : 0;
    for (size_t i = 0; i < n; i++)
        buf_append(&c->meta, &meta, 1);
}

/* Cells reuse inline parsing without block handling or wrapping. bold_base suppresses inner bold
 * toggles that would otherwise cancel an already-bold header or reflow label. */
static void render_cell(const struct md_table_context *ctx, struct cell *c, const char *text,
                        size_t len, int bold_base)
{
    buf_init(&c->bytes);
    buf_init(&c->meta);
    c->width = 0;
    ctx->render_inline(ctx->user, text, len, bold_base, cell_emit, c);
    /* Measure assembled content runs so callback splits cannot divide a codepoint. */
    size_t i = 0;
    while (i < c->bytes.len) {
        if (c->meta.data[i] == 0) {
            size_t j = i;
            while (j < c->bytes.len && c->meta.data[j] == 0)
                j++;
            c->width += cell_visible_width(c->bytes.data + i, j - i);
            i = j;
        } else {
            i++;
        }
    }
}

static void cell_clear(struct cell *c)
{
    buf_free(&c->bytes);
    buf_free(&c->meta);
}

/* Preserve raw/content callback kinds while coalescing adjacent bytes. */
static void emit_cell_bytes(const struct md_table_context *ctx, const struct cell *c)
{
    size_t i = 0;
    while (i < c->bytes.len) {
        char kind = c->meta.data[i];
        size_t j = i + 1;
        while (j < c->bytes.len && c->meta.data[j] == kind)
            j++;
        ctx->emit_direct(c->bytes.data + i, j - i, kind ? 1 : 0, ctx->user);
        i = j;
    }
}

/* Reflow replays content through wrapping and cell escapes through semantic
 * style restoration, so spans crossing a break resume on the continuation row.
 * Cell styles are balanced and return to their entry state. */
static void emit_cell_wrapped(const struct md_table_context *ctx, const struct cell *c)
{
    size_t i = 0;
    while (i < c->bytes.len) {
        char kind = c->meta.data[i];
        size_t j = i + 1;
        while (j < c->bytes.len && c->meta.data[j] == kind)
            j++;
        if (kind)
            ctx->replay_raw(ctx->user, c->bytes.data + i, j - i);
        else
            emit_wrapped_text(ctx, c->bytes.data + i, j - i);
        i = j;
    }
}

/* Pad to colw without trailing whitespace after the final column. */
static void emit_cell(const struct md_table_context *ctx, const struct cell *c, int colw,
                      char align, int bold, int last)
{
    int pad = colw - c->width;
    if (pad < 0)
        pad = 0;
    int padl = 0, padr = 0;
    if (align == 'R')
        padl = pad;
    else if (align == 'C') {
        padl = pad / 2;
        padr = pad - padl;
    } else
        padr = pad;
    if (padl)
        emit_direct_spaces(ctx, padl);
    if (bold)
        emit_direct_raw(ctx, ANSI_BOLD);
    emit_cell_bytes(ctx, c);
    if (bold)
        emit_direct_raw(ctx, ANSI_BOLD_OFF);
    if (!last && padr)
        emit_direct_spaces(ctx, padr);
}

/* Dim three-cell separator between aligned columns. */
static void emit_col_sep(const struct md_table_context *ctx)
{
    emit_direct_text(ctx, " ", 1);
    emit_direct_raw(ctx, ANSI_DIM);
    emit_direct_text(ctx, GLYPH_VLINE, 3);
    emit_direct_raw(ctx, ANSI_BOLD_OFF);
    emit_direct_text(ctx, " ", 1);
}

/* Split a row into trimmed source spans, ignoring optional border pipes. Literal pipes are not
 * decoded here; extra body cells trigger lossless fallback in finalize_table(). */
static int split_row(const char *s, size_t len, const char **cp, size_t *clen, int max)
{
    size_t a = 0, b = len;
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
        a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        b--;
    if (a < b && s[a] == '|')
        a++; /* leading border */
    if (b > a && s[b - 1] == '|')
        b--; /* trailing border */
    int n = 0;
    size_t start = a;
    for (size_t i = a; i <= b; i++) {
        if (i == b || s[i] == '|') {
            size_t ca = start, cb = i;
            while (ca < cb && (s[ca] == ' ' || s[ca] == '\t'))
                ca++;
            while (cb > ca && (s[cb - 1] == ' ' || s[cb - 1] == '\t'))
                cb--;
            if (n < max) {
                cp[n] = s + ca;
                clen[n] = cb - ca;
            }
            n++;
            start = i + 1;
        }
    }
    return n;
}

/* Return the number of `:?-+:?` delimiter cells, or zero. Colons are valid only at cell edges. */
static int is_delimiter_row(const char *line, size_t len)
{
    const char *cp[TABLE_MAX_COLS];
    size_t cl[TABLE_MAX_COLS];
    int n = split_row(line, len, cp, cl, TABLE_MAX_COLS);
    if (n < 1 || n > TABLE_MAX_COLS)
        return 0;
    for (int j = 0; j < n; j++) {
        const char *s = cp[j];
        size_t a = 0, b = cl[j];
        if (b == 0)
            return 0;
        if (s[a] == ':') /* optional leading colon */
            a++;
        if (b > a && s[b - 1] == ':') /* optional trailing colon */
            b--;
        if (a >= b) /* need at least one dash between the colons */
            return 0;
        for (size_t k = a; k < b; k++)
            if (s[k] != '-')
                return 0;
    }
    return n;
}

/* GFM requires the header and valid delimiter row to have equal column counts. */
static int table_header_matches_delim(const char *h, size_t hlen, const char *d, size_t dlen)
{
    const char *cp[TABLE_MAX_COLS];
    size_t cl[TABLE_MAX_COLS];
    int hcols = split_row(h, hlen, cp, cl, TABLE_MAX_COLS);
    int dcols = is_delimiter_row(d, dlen);
    return hcols >= 1 && hcols <= TABLE_MAX_COLS && dcols == hcols;
}

/* Emit an aligned grid when its natural width fits; otherwise reflow body rows into bulleted
 * label/value records that can wrap at any width. */
static void finalize_table(struct md_table *t, const struct md_table_context *ctx)
{
    struct buf *tb = &t->buf;
    ctx->commit_pending(ctx->user);

    /* Gather line spans (newline-terminated; the table buffer always ends \n). */
    const char *ls[TABLE_MAX_ROWS];
    size_t ll[TABLE_MAX_ROWS];
    int lc = 0;
    size_t s0 = 0;
    for (size_t k = 0; k < tb->len && lc < TABLE_MAX_ROWS; k++) {
        if (tb->data[k] == '\n') {
            ls[lc] = tb->data + s0;
            ll[lc] = k - s0;
            lc++;
            s0 = k + 1;
        }
    }
    if (lc >= TABLE_MAX_ROWS && s0 < tb->len) {
        /* More rows than the span array holds — don't silently drop the
         * tail; emit the whole block verbatim through the wrap path. */
        emit_wrapped_text(ctx, tb->data, tb->len);
        goto done;
    }

    const char *hcp[TABLE_MAX_COLS];
    size_t hcl[TABLE_MAX_COLS];
    int ncols = lc >= 1 ? split_row(ls[0], ll[0], hcp, hcl, TABLE_MAX_COLS) : 0;
    if (ncols > TABLE_MAX_COLS)
        ncols = TABLE_MAX_COLS;
    if (lc < 2 || ncols < 1) {
        /* Malformed / too small to lay out — emit verbatim through the
         * normal wrap path so nothing is lost. */
        emit_wrapped_text(ctx, tb->data, tb->len);
        goto done;
    }

    /* A literal `|` over-splits because split_row() does not parse escapes or code spans. Never
     * drop those extra cells: preserve the whole table through wrapped fallback. */
    for (int r = 2; r < lc; r++) {
        const char *bcp[TABLE_MAX_COLS];
        size_t bcl[TABLE_MAX_COLS];
        if (split_row(ls[r], ll[r], bcp, bcl, TABLE_MAX_COLS) > ncols) {
            emit_wrapped_text(ctx, tb->data, tb->len);
            goto done;
        }
    }

    /* Column alignment from the delimiter row (`:--` left, `--:` right,
     * `:-:` center; default left). */
    const char *dcp[TABLE_MAX_COLS];
    size_t dcl[TABLE_MAX_COLS];
    int ndelim = split_row(ls[1], ll[1], dcp, dcl, TABLE_MAX_COLS);
    char align[TABLE_MAX_COLS];
    for (int j = 0; j < ncols; j++) {
        char a = 'L';
        if (j < ndelim && dcl[j] > 0) {
            int lcolon = dcp[j][0] == ':';
            int rcolon = dcp[j][dcl[j] - 1] == ':';
            a = (lcolon && rcolon) ? 'C' : rcolon ? 'R' : 'L';
        }
        align[j] = a;
    }

    int nbody = lc - 2;
    int nrows = 1 + nbody; /* header + body */
    struct cell *grid = xcalloc((size_t)nrows * ncols, sizeof(*grid));

    /* Header cells inherit the grid's outer bold. */
    for (int j = 0; j < ncols; j++)
        render_cell(ctx, &grid[j], hcp[j], hcl[j], 1);
    /* Missing body cells render empty; extra cells already fell back above. */
    for (int r = 0; r < nbody; r++) {
        const char *bcp[TABLE_MAX_COLS];
        size_t bcl[TABLE_MAX_COLS];
        int got = split_row(ls[2 + r], ll[2 + r], bcp, bcl, TABLE_MAX_COLS);
        for (int j = 0; j < ncols; j++) {
            struct cell *cell = &grid[(size_t)(1 + r) * ncols + j];
            if (j < got)
                render_cell(ctx, cell, bcp[j], bcl[j], 0);
            else
                render_cell(ctx, cell, "", 0, 0);
        }
    }

    int colw[TABLE_MAX_COLS];
    int total = 0;
    for (int j = 0; j < ncols; j++) {
        int w = 1; /* keep an empty column visible */
        for (int r = 0; r < nrows; r++) {
            int cw = grid[(size_t)r * ncols + j].width;
            if (cw > w)
                w = cw;
        }
        colw[j] = w;
        total += w;
    }
    total += (ncols - 1) * TABLE_COL_SEP;

    /* Header-only tables cannot become records, so keep their grid even when it overflows. */
    if (ctx->wrap_width <= 0 || total <= ctx->wrap_width || nbody == 0) {
        /* Header, crossing rule, then body rows. */
        for (int j = 0; j < ncols; j++) {
            emit_cell(ctx, &grid[j], colw[j], align[j], 1, j == ncols - 1);
            if (j < ncols - 1)
                emit_col_sep(ctx);
        }
        emit_direct_text(ctx, "\n", 1);
        emit_direct_raw(ctx, ANSI_DIM);
        for (int j = 0; j < ncols; j++) {
            emit_direct_glyphs(ctx, colw[j], GLYPH_HLINE);
            if (j < ncols - 1) {
                emit_direct_glyphs(ctx, 1, GLYPH_HLINE); /* ─ under the left pad space */
                emit_direct_text(ctx, GLYPH_CROSS, 3);   /* ┼ under the │ */
                emit_direct_glyphs(ctx, 1, GLYPH_HLINE); /* ─ under the right pad space */
            }
        }
        emit_direct_raw(ctx, ANSI_BOLD_OFF);
        emit_direct_text(ctx, "\n", 1);
        for (int r = 0; r < nbody; r++) {
            for (int j = 0; j < ncols; j++) {
                emit_cell(ctx, &grid[(size_t)(1 + r) * ncols + j], colw[j], align[j], 0,
                          j == ncols - 1);
                if (j < ncols - 1)
                    emit_col_sep(ctx);
            }
            emit_direct_text(ctx, "\n", 1);
        }
    } else {
        /* Reflow each body row into bulleted label/value lines. Every line starts with a two-cell
         * prefix, which also establishes the hanging indent for wrapped values. */
        for (int r = 0; r < nbody; r++) {
            struct cell *row = &grid[(size_t)(1 + r) * ncols];
            for (int j = 0; j < ncols; j++) {
                if (j == 0)
                    emit_wrapped_bullet(ctx); /* dim "• " */
                else
                    emit_wrapped_text(ctx, "  ", 2);
                ctx->open_bold(ctx->user);        /* tracked, so a wrapped label keeps bold */
                emit_cell_wrapped(ctx, &grid[j]); /* header label */
                ctx->close_bold(ctx->user);
                emit_wrapped_text(ctx, ": ", 2);
                emit_cell_wrapped(ctx, &row[j]);
                emit_wrapped_text(ctx, "\n", 1);
            }
        }
    }

    for (int r = 0; r < nrows; r++)
        for (int j = 0; j < ncols; j++)
            cell_clear(&grid[(size_t)r * ncols + j]);
    free(grid);

done:
    md_table_reset(t);
    /* Every table path ends on complete lines, leaving no reusable row shadow. */
    ctx->row_reset(ctx->user);
}

enum md_table_result md_table_step(struct md_table *t, const struct md_table_context *ctx,
                                   const struct buf *work, size_t *offset)
{
    size_t nl = *offset;
    while (nl < work->len && work->data[nl] != '\n')
        nl++;
    if (nl >= work->len)
        return MD_TABLE_DEFER;

    int blank = 1, pipe = 0;
    for (size_t k = *offset; k < nl; k++) {
        char ch = work->data[k];
        if (ch != ' ' && ch != '\t' && ch != '\r')
            blank = 0;
        if (ch == '|')
            pipe = 1;
    }
    size_t line_len = nl - *offset + 1;
    /* Count the incoming row so one complete oversized row also bails. */
    if (blank || !pipe || t->buf.len + line_len > TABLE_MAX_BYTES) {
        finalize_table(t, ctx);
        return MD_TABLE_PASS;
    }
    buf_append(&t->buf, work->data + *offset, line_len);
    *offset = nl + 1;
    return MD_TABLE_ADVANCED;
}

enum md_table_result md_table_try_start(struct md_table *t, const struct buf *work, size_t *offset)
{
    size_t nl1 = *offset;
    while (nl1 < work->len && work->data[nl1] != '\n')
        nl1++;
    if (nl1 >= work->len)
        return MD_TABLE_DEFER;
    size_t nl2 = nl1 + 1;
    while (nl2 < work->len && work->data[nl2] != '\n')
        nl2++;
    if (nl2 >= work->len)
        return MD_TABLE_DEFER;
    if (nl2 - *offset + 1 > TABLE_MAX_BYTES)
        return MD_TABLE_PASS;
    if (!table_header_matches_delim(work->data + *offset, nl1 - *offset, work->data + nl1 + 1,
                                    nl2 - (nl1 + 1)))
        return MD_TABLE_PASS;
    buf_append(&t->buf, work->data + *offset, nl2 - *offset + 1);
    t->collecting = 1;
    *offset = nl2 + 1;
    return MD_TABLE_ADVANCED;
}

int md_table_bail_partial(struct md_table *t, const struct md_table_context *ctx,
                          const char *partial, size_t len)
{
    if (!t->collecting || len <= TABLE_MAX_BYTES)
        return 0;
    emit_wrapped_text(ctx, t->buf.data, t->buf.len);
    md_table_reset(t);
    emit_wrapped_text(ctx, partial, len);
    return 1;
}

void md_table_finish(struct md_table *t, const struct md_table_context *ctx, struct buf *tail)
{
    if (!t->collecting && tail->len > 0 && tail->data[0] == '|') {
        size_t nl = 0;
        while (nl < tail->len && tail->data[nl] != '\n')
            nl++;
        if (nl < tail->len && tail->len + 1 <= TABLE_MAX_BYTES &&
            table_header_matches_delim(tail->data, nl, tail->data + nl + 1, tail->len - (nl + 1))) {
            buf_append(&t->buf, tail->data, tail->len);
            buf_append(&t->buf, "\n", 1);
            buf_reset(tail);
            t->collecting = 1;
        }
    }

    if (!t->collecting)
        return;

    int blank = 1, pipe = 0;
    for (size_t k = 0; k < tail->len; k++) {
        char ch = tail->data[k];
        if (ch != ' ' && ch != '\t' && ch != '\r')
            blank = 0;
        if (ch == '|')
            pipe = 1;
    }
    if (tail->len > 0 && !blank && pipe && t->buf.len + tail->len + 1 <= TABLE_MAX_BYTES) {
        buf_append(&t->buf, tail->data, tail->len);
        buf_append(&t->buf, "\n", 1);
        buf_reset(tail);
    }
    finalize_table(t, ctx);
}
