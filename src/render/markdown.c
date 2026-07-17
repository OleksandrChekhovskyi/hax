/* SPDX-License-Identifier: MIT */
#include "render/markdown.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "render/markdown_wrap.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"
#include "text/utf8.h"

/* Sanity bound on the deferred-tail size. Inline markers fit in a few bytes;
 * the only thing that can grow large is a partial fence opener whose info
 * string spans multiple deltas. Realistic openers are well under 1 KiB even
 * with attributes; this cap prevents runaway growth on malformed input. If
 * exceeded, the excess is flushed as plain text. */
#define TAIL_MAX 8192

struct md_renderer {
    md_emit_fn emit_cb;
    void *user;

    /* Bytes deferred from a previous feed because they could form a
     * multi-byte marker that needs lookahead. Grows dynamically so a
     * long fence opener line that spans deltas (e.g. `` ```python
     * title="..." ``) doesn't get its leading backticks flushed as
     * literal text and unbalance subsequent fence parsing. */
    struct buf tail;

    /* Last source byte processed in any prior feed — needed for the
     * intraword check on a marker at position 0 of the current work
     * buffer (e.g. feed("foo") then feed("_bar_") must NOT open italic
     * because the `_` has `o` to its left in the source stream). */
    char prev_byte;

    int at_line_start;

    /* Run of space bytes at the current end-of-line, reset by any
     * non-space content (in emit_text) and by every consumed inline
     * delimiter (in the open_/close_ helpers). Lets the \n handler spot
     * a CommonMark hard line break (2+ trailing spaces) without looking
     * back into the work buffer — so the rule still fires when the
     * spaces and the newline land in separate feeds (e.g. a provider
     * that streams "  " and "\n" as two deltas), yet stays accurate when
     * the spaces precede a delimiter (`foo  **\n`) rather than the line
     * ending. */
    int trailing_spaces;

    /* Number of backticks in the active fence's opener — closer must have
     * at least this many, with no info string after, per CommonMark. Lets
     * a ```markdown demo containing inner ```python lines stay open until
     * a *bare* ``` line. */
    size_t fence_open_count;

    /* Style flags. All independent — each maps to a distinct SGR group, so
     * e.g. an inline-code span inside bold leaves bold intact when the code
     * span closes. The one exception is heading-vs-inline-bold which both
     * use the same SGR; close_bold re-emits ANSI_BOLD if in_heading is
     * still set so the rest of the heading line stays bold. */
    int in_heading;
    int in_code_fence;
    int in_inline_code;
    int in_bold;
    int in_italic;

    /* When 0, the open_* / close_* helpers below suppress their SGR
     * escapes but keep the in_* flag bookkeeping. The parser still
     * tracks bold/italic/etc. correctly (so markers nest and close as
     * usual) — only the visible color changes are dropped. Wrap and
     * block structure are unaffected. Defaults to 1. See md_set_styled. */
    int styled;

    /* Set when the current line starts with a marker that isolates it
     * from surrounding prose (blockquote `>`, GFM table row `|`). The
     * trailing \n of such a line is forced hard regardless of what's
     * on the next line — so a blockquote line followed by plain
     * prose doesn't soft-join into a single paragraph. Cleared on
     * every hard \n; set in step_line_start when the leading byte
     * matches. */
    int cur_line_is_block;

    /* Set while output sits at a blank-line boundary — a blank line has
     * just been emitted, or nothing has been emitted yet. Extra
     * consecutive blank lines are then suppressed so a run collapses to a
     * single one; models sometimes emit several, which reads as ragged
     * vertical gaps in the terminal. Set when the one allowed blank line
     * is emitted in step_line_start, cleared there as soon as a non-blank
     * line is processed. Only prose blank lines route through that path —
     * code-fence and table interiors bypass it, so their verbatim blank
     * lines are preserved. */
    int at_blank;

    /* Set when a list marker consumed a space run that reached the buffer
     * end: leading spaces at the head of the next feed are the tail of that
     * marker's padding and must be dropped, not rendered as content, so the
     * marker still collapses to one space across the boundary. Cleared by
     * the first non-space byte in step_inline. */
    int skip_pad;

    struct md_wrap wrap;

    /* ---------- table buffer ----------
     *
     * Tables are the one block type that can't be rendered as it streams:
     * column widths and the fit-or-reflow decision need every row first.
     * So when a `|`-led line is confirmed to head a GFM table (its next
     * line is a delimiter row), the renderer diverts subsequent lines into
     * table_buf until a blank / non-pipe line / end-of-stream, then lays
     * the whole block out at once (finalize_table). in_table gates the
     * divert in md_feed. wrap_width <= 0 lays the grid out at natural
     * width (unlimited) rather than reflowing. */
    int in_table;
    struct buf table_buf;

    /* Inline-only mode: skip all line-start block parsing (headings,
     * lists, fences, rules, tables) so the input is treated as a single
     * run of inline Markdown. Used by render_cell — GFM table cells are
     * inline contexts, so `# H`, `- x`, `---`, ``` ``` ``` inside a cell
     * must stay literal text, not become block constructs. */
    int inline_only;

    /* Suppress bold SGR (open_bold/close_bold emit nothing) while still
     * tracking the in_bold flag for marker nesting. Set on the sub-renderer
     * for a header / reflow-label cell, whose surrounding context already
     * applies bold — so an inner `**...**` span doesn't emit a bold-off
     * that cancels the outer header bold for the rest of the cell. */
    int suppress_bold;
};

static int is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static struct md_wrap_context wrap_context(const struct md_renderer *m)
{
    return (struct md_wrap_context){
        .emit = m->emit_cb,
        .user = m->user,
        .styled = m->styled,
        .in_bold = m->in_bold,
        .in_italic = m->in_italic,
        .in_inline_code = m->in_inline_code,
    };
}

static void emit_text(struct md_renderer *m, const char *s, size_t n)
{
    /* Track the source bytes before the wrap layer substitutes tabs. */
    for (size_t j = 0; j < n; j++) {
        if (s[j] == ' ')
            m->trailing_spaces++;
        else if (s[j] != '\r')
            m->trailing_spaces = 0;
    }

    struct md_wrap_context ctx = wrap_context(m);
    int verbatim = m->in_code_fence || m->in_heading;
    md_wrap_emit_text(&m->wrap, &ctx, s, n, verbatim);
}

/* Emit an ANSI escape (zero-width). */
static void emit_raw(struct md_renderer *m, const char *s)
{
    if (!m->styled)
        return;
    struct md_wrap_context ctx = wrap_context(m);
    int verbatim = m->in_code_fence || m->in_heading;
    md_wrap_emit_raw(&m->wrap, &ctx, s, strlen(s), verbatim);
}

/* An inline delimiter (`*`, `**`, `_`, `` ` ``) is a non-space source
 * byte that's consumed without passing through emit_text, so it must
 * clear the trailing-space run itself — otherwise spaces sitting before
 * the delimiter (e.g. `foo  **`) would leave a stale count and a later
 * \n would wrongly read as a hard break. */
static void open_bold(struct md_renderer *m)
{
    if (!m->suppress_bold)
        emit_raw(m, ANSI_BOLD);
    m->in_bold = 1;
    m->trailing_spaces = 0;
}

static void close_bold(struct md_renderer *m)
{
    if (!m->suppress_bold)
        emit_raw(m, ANSI_BOLD_OFF);
    m->in_bold = 0;
    m->trailing_spaces = 0;
    /* Re-assert the heading style: SGR 22 above closed its bold, and a
     * themed heading open may carry a color too (re-opening is idempotent). */
    if (m->in_heading && !m->suppress_bold)
        emit_raw(m, theme_open(THEME_HEADING));
}

static void open_italic(struct md_renderer *m)
{
    emit_raw(m, ANSI_ITALIC);
    m->in_italic = 1;
    m->trailing_spaces = 0;
}

static void close_italic(struct md_renderer *m)
{
    emit_raw(m, ANSI_ITALIC_OFF);
    m->in_italic = 0;
    m->trailing_spaces = 0;
}

static void open_inline_code(struct md_renderer *m)
{
    emit_raw(m, theme_open(THEME_CODE_INLINE));
    m->in_inline_code = 1;
    m->trailing_spaces = 0;
}

static void close_inline_code(struct md_renderer *m)
{
    emit_raw(m, theme_close(THEME_CODE_INLINE));
    m->in_inline_code = 0;
    m->trailing_spaces = 0;
    /* The code closer restored the default foreground; when a themed
     * heading carries a color of its own (open != plain bold), bring
     * it back for the rest of the heading line. */
    if (m->in_heading && strcmp(theme_open(THEME_HEADING), ANSI_BOLD) != 0)
        emit_raw(m, theme_open(THEME_HEADING));
}

static void open_code_fence(struct md_renderer *m)
{
    /* Flip the flag before emitting so emit_raw takes the bypass
     * path (escape goes direct to emit_cb instead of buffering into
     * row_buf with content that gets re-emitted on a wrap break). */
    m->in_code_fence = 1;
    emit_raw(m, theme_open(THEME_CODE_BLOCK));
}

static void close_code_fence(struct md_renderer *m)
{
    emit_raw(m, theme_close(THEME_CODE_BLOCK));
    m->in_code_fence = 0;
}

static void open_heading(struct md_renderer *m)
{
    /* Flag-first, see open_code_fence for rationale. */
    m->in_heading = 1;
    emit_raw(m, theme_open(THEME_HEADING));
}

static void close_heading(struct md_renderer *m)
{
    emit_raw(m, theme_close(THEME_HEADING));
    m->in_heading = 0;
}

/* Per-state byte-dispatch outcome (used by the table collectors below and
 * the step machine further down). STEP_ADVANCED: consumed bytes, advanced
 * *i. STEP_DEFER: not enough bytes to decide. STEP_PASS: handler didn't
 * apply; try the next. */
enum step_result {
    STEP_ADVANCED,
    STEP_DEFER,
    STEP_PASS,
};

/* ---------- thematic breaks and tables ----------
 *
 * Both render whole lines at once that don't participate in inline wrap,
 * so they bypass the wrap layer and write complete rows straight to
 * emit_cb. A thematic break is one styled line; a table is a buffered
 * block laid out together. The temit_* helpers below are the direct-emit
 * primitives they share — content via emit_cb(is_raw=0), SGR via
 * emit_cb(is_raw=1) gated on m->styled (same suppression rule as
 * emit_raw, so a table inside an unstyled CoT span stays uncolored). */

#define TABLE_COL_SEP   3         /* width of the " │ " column separator in cells */
#define TABLE_MAX_COLS  32        /* wider headers fall back to verbatim passthrough */
#define TABLE_MAX_ROWS  2048      /* row cap; excess rows are dropped from layout */
#define TABLE_MAX_BYTES (1 << 16) /* buffer cap; an over-long block bails to text */

/* The model's thematic-break divider is a short row of spaced dim dots (a
 * typographic "dinkus"), so it reads as a quiet content separator rather
 * than competing with the harness's own structural chrome — the dim SOLID
 * full-width `─` rules in transcript.c and the `── resumed ──` marker in
 * agent.c. The convention: solid rule = system, spaced dots = model
 * divider. Tables use the solid line family for their internal grid. */
#define HRULE_DOT_GAP 3              /* spaces between divider dots */
#define GLYPH_DOT     "\xc2\xb7"     /* · middle dot — model divider */
#define GLYPH_BULLET  "\xe2\x80\xa2" /* • bullet — reflowed-table record marker */
#define GLYPH_HLINE   "\xe2\x94\x80" /* ─ light horizontal — table rules */
#define GLYPH_VLINE   "\xe2\x94\x82" /* │ light vertical — table column sep */
#define GLYPH_CROSS   "\xe2\x94\xbc" /* ┼ light cross — table underline join */

static void temit(struct md_renderer *m, const char *s, size_t n)
{
    if (n)
        m->emit_cb(s, n, 0, m->user);
}

static void temit_raw(struct md_renderer *m, const char *s)
{
    if (m->styled)
        m->emit_cb(s, strlen(s), 1, m->user);
}

static void temit_spaces(struct md_renderer *m, int n)
{
    static const char SP[] = "                                ";
    while (n > 0) {
        int k = n > 32 ? 32 : n;
        temit(m, SP, (size_t)k);
        n -= k;
    }
}

/* Emit n copies of a 3-byte box-drawing glyph as content. Used for the
 * thematic-break divider (dashed) and the table grid lines (solid). */
static void temit_glyphs(struct md_renderer *m, int n, const char *g3)
{
    char buf[96]; /* 32 glyphs * 3 bytes */
    while (n > 0) {
        int k = n > 32 ? 32 : n;
        int p = 0;
        for (int x = 0; x < k; x++) {
            buf[p++] = g3[0];
            buf[p++] = g3[1];
            buf[p++] = g3[2];
        }
        temit(m, buf, (size_t)p);
        n -= k;
    }
}

/* Render a thematic break (`---` / `***` / `___`) as three spaced dim
 * dots — a quiet content divider, even by construction and distinct from
 * the harness's full-width solid rules. On a very narrow terminal it
 * drops dots so it never wraps; wrap_width <= 0 means unlimited (3 dots). */
static void render_hrule(struct md_renderer *m)
{
    int dots = 3;
    while (md_wrap_width(&m->wrap) > 0 && dots > 1 &&
           dots + (dots - 1) * HRULE_DOT_GAP > md_wrap_width(&m->wrap))
        dots--;
    temit_raw(m, ANSI_DIM);
    for (int k = 0; k < dots; k++) {
        if (k)
            temit_spaces(m, HRULE_DOT_GAP);
        temit(m, GLYPH_DOT, 2);
    }
    temit_raw(m, ANSI_BOLD_OFF); /* SGR 22 closes dim */
    temit(m, "\n", 1);
}

/* If s[start..end) is a thematic-break line — 3+ of a single `-`, `*`, or
 * `_`, with only whitespace between — return that marker char, else 0.
 * `=` is deliberately excluded: it's only ever a setext underline, which
 * stays literal. Used by md_flush to render a thematic break that ends the
 * stream without a trailing newline as the dim divider, matching the
 * streaming path (which needs the \n to disambiguate but the EOF bytes
 * are the whole line). */
static char eof_thematic_marker(const char *s, size_t start, size_t end)
{
    if (start >= end)
        return 0;
    char first = s[start];
    if (first != '-' && first != '*' && first != '_')
        return 0;
    size_t count = 0;
    for (size_t j = start; j < end; j++) {
        if (s[j] == first)
            count++;
        else if (s[j] != ' ' && s[j] != '\t' && s[j] != '\r')
            return 0;
    }
    return count >= 3 ? first : 0;
}

/* Emit a dim "• " list bullet through the wrap layer (content path, so it
 * lands in row_buf for compute_indent_cells and wraps with the item).
 * Shared by the markdown list-marker conversion and the table reflow. */
static void emit_bullet(struct md_renderer *m)
{
    emit_raw(m, ANSI_DIM);
    emit_text(m, GLYPH_BULLET " ", 4);
    emit_raw(m, ANSI_BOLD_OFF); /* SGR 22 closes dim */
}

/* Visible (terminal-cell) width of a content byte run, summing wcwidth-
 * style measurements. Non-printables (already filtered by the cell sub-
 * renderer) clamp to 0; combining marks add 0. */
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

/* One rendered table cell: the styled byte stream (content interleaved
 * with SGR escapes), a parallel meta array (1 = raw escape, 0 = content)
 * so it can be re-emitted with correct is_raw flags, and the visible
 * width in cells (computed from the assembled content runs). */
struct cell {
    struct buf bytes;
    struct buf meta;
    int width;
};

/* emit_cb sink for the per-cell sub-renderer: accumulate styled bytes +
 * parallel meta (1 = raw escape, 0 = content). Width is NOT tallied here —
 * inline code emits content byte-by-byte, so a multibyte codepoint would
 * arrive split across calls and measure as 0; render_cell measures the
 * assembled content runs instead (see below). */
static void cell_emit(const char *bytes, size_t n, int is_raw, void *user)
{
    struct cell *c = user;
    buf_append(&c->bytes, bytes, n);
    char meta = is_raw ? 1 : 0;
    for (size_t i = 0; i < n; i++)
        buf_append(&c->meta, &meta, 1);
}

/* Render one cell's markdown into styled bytes by running a nested,
 * wrap-disabled md_renderer over it — reusing the whole inline engine so
 * `**bold**`, `_italic_`, and `` `code` `` inside a cell render correctly.
 * Cells are single-line (split on `|`), so no block structure or wrapping
 * is in play. Inherits the outer styled setting. bold_base marks a cell
 * whose surrounding context is already bold (a header cell / reflow
 * label): its own `**...**` toggles are suppressed so an inner span's
 * bold-off can't cancel the outer header bold for the rest of the cell. */
static void render_cell(struct cell *c, const char *text, size_t len, int styled, int bold_base)
{
    buf_init(&c->bytes);
    buf_init(&c->meta);
    c->width = 0;
    struct md_renderer *sub = md_new(cell_emit, c, 0);
    if (!styled)
        md_set_styled(sub, 0); /* calls md_reset; set flags after */
    sub->inline_only = 1;      /* cells are inline contexts — no block markers */
    sub->suppress_bold = bold_base;
    md_feed(sub, text, len);
    md_flush(sub);
    md_free(sub);
    /* Measure width from the assembled content runs (meta == 0): inline
     * code emits content byte-by-byte, so per-chunk measurement in
     * cell_emit would undercount a multibyte codepoint split across calls.
     * A content run holds whole codepoints (no escape splits one, since
     * the cell renderer runs wrap-disabled). */
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

/* Re-emit a cell's styled bytes, splitting into runs of like meta so
 * escapes go out is_raw=1 and content is_raw=0 — same split discipline as
 * wrap_flush_range. */
static void emit_cell_bytes(struct md_renderer *m, const struct cell *c)
{
    size_t i = 0;
    while (i < c->bytes.len) {
        char kind = c->meta.data[i];
        size_t j = i + 1;
        while (j < c->bytes.len && c->meta.data[j] == kind)
            j++;
        m->emit_cb(c->bytes.data + i, j - i, kind ? 1 : 0, m->user);
        i = j;
    }
}

/* Replay a counted escape run through the wrap layer (is_raw=1, zero
 * width). Like emit_raw but for non-NUL-terminated bytes; used by the
 * reflow path, which always runs in wrap mode. */
static void emit_raw_run(struct md_renderer *m, const char *s, size_t n)
{
    if (!m->styled || n == 0)
        return;
    struct md_wrap_context ctx = wrap_context(m);
    md_wrap_emit_raw(&m->wrap, &ctx, s, n, 0);
}

/* Mirror a replayed cell SGR run into the renderer's in_* style flags so
 * the wrap layer's break snapshot (snap_in_*) captures the right state: a
 * value that wraps mid-span must re-assert the style on the continuation
 * row, and wrap_break reads it from these flags. A run can hold several
 * coalesced sequences; each cell SGR ends in 'm', so split on that. (Cells
 * only ever emit the bold/italic/inline-code pairs.) */
static void cell_track_sgr(struct md_renderer *m, const char *s, size_t n)
{
    size_t p = 0;
    while (p < n) {
        size_t q = p;
        while (q < n && s[q] != 'm')
            q++;
        if (q < n)
            q++; /* include the final 'm' */
        size_t len = q - p;
        const char *code_on = theme_open(THEME_CODE_INLINE);
        const char *code_off = theme_close(THEME_CODE_INLINE);
        if (len == strlen(ANSI_BOLD) && !memcmp(s + p, ANSI_BOLD, len))
            m->in_bold = 1;
        else if (len == strlen(ANSI_BOLD_OFF) && !memcmp(s + p, ANSI_BOLD_OFF, len))
            m->in_bold = 0;
        else if (len == strlen(ANSI_ITALIC) && !memcmp(s + p, ANSI_ITALIC, len))
            m->in_italic = 1;
        else if (len == strlen(ANSI_ITALIC_OFF) && !memcmp(s + p, ANSI_ITALIC_OFF, len))
            m->in_italic = 0;
        else if (len == strlen(code_on) && !memcmp(s + p, code_on, len))
            m->in_inline_code = 1;
        else if (len == strlen(code_off) && !memcmp(s + p, code_off, len))
            m->in_inline_code = 0;
        p = q;
    }
}

/* Re-emit a cell's styled bytes through the wrap engine: content runs go
 * through emit_text (so they word-wrap with the row's continuation indent)
 * and escape runs through emit_raw_run, tracking the active style so a
 * span crossing a wrap point is re-applied on the continuation row (via
 * wrap_break's SGR-rewind). Used by the reflow path. Cell SGR is balanced
 * (the sub-renderer's md_flush closes every span), so in_* returns to its
 * entry state after the cell. */
static void emit_cell_wrapped(struct md_renderer *m, const struct cell *c)
{
    size_t i = 0;
    while (i < c->bytes.len) {
        char kind = c->meta.data[i];
        size_t j = i + 1;
        while (j < c->bytes.len && c->meta.data[j] == kind)
            j++;
        if (kind) {
            cell_track_sgr(m, c->bytes.data + i, j - i);
            emit_raw_run(m, c->bytes.data + i, j - i);
        } else
            emit_text(m, c->bytes.data + i, j - i);
        i = j;
    }
}

/* Emit a cell padded to colw per its alignment. `last` suppresses the
 * right pad on the final column so no trailing whitespace is written.
 * `bold` wraps the content in bold (used for header cells). */
static void emit_cell(struct md_renderer *m, const struct cell *c, int colw, char align, int bold,
                      int last)
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
        temit_spaces(m, padl);
    if (bold)
        temit_raw(m, ANSI_BOLD);
    emit_cell_bytes(m, c);
    if (bold)
        temit_raw(m, ANSI_BOLD_OFF);
    if (!last && padr)
        temit_spaces(m, padr);
}

/* The dim ` │ ` separator between two columns in a header/body row
 * (TABLE_COL_SEP cells wide). */
static void emit_col_sep(struct md_renderer *m)
{
    temit(m, " ", 1);
    temit_raw(m, ANSI_DIM);
    temit(m, GLYPH_VLINE, 3);
    temit_raw(m, ANSI_BOLD_OFF);
    temit(m, " ", 1);
}

/* Split a single table-row line (no trailing newline) into trimmed cell
 * substrings pointing into the source. Handles the optional leading and
 * trailing border pipes. Splits on every `|` — no awareness of inline
 * code spans or escaped `\|`, so a cell with a literal pipe over-splits;
 * finalize_table guards against that by bailing the whole table to
 * verbatim when a body row yields more cells than the header. Returns the
 * cell count; fills up to `max`. */
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

/* If `line` is a GFM table delimiter row, return its cell count, else 0.
 * Each (space-trimmed) cell must be exactly `:?-+:?` — an optional leading
 * colon, one or more dashes, an optional trailing colon, and nothing else
 * (no interior colons or spaces). GFM only allows colons at the cell
 * edges, so `:-:-` / `-:-` are not delimiter rows. (Count, not bool, so
 * callers can match it against the header's column count.) */
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

/* True if a (header line, delimiter line) pair forms a GFM table: the
 * delimiter is valid and has the SAME cell count as the header. GFM
 * doesn't recognize a table whose rows disagree on column count, so a
 * mismatch is rejected (the caller passes the lines through verbatim)
 * rather than partially formatted with dropped columns. */
static int table_header_matches_delim(const char *h, size_t hlen, const char *d, size_t dlen)
{
    const char *cp[TABLE_MAX_COLS];
    size_t cl[TABLE_MAX_COLS];
    int hcols = split_row(h, hlen, cp, cl, TABLE_MAX_COLS);
    int dcols = is_delimiter_row(d, dlen);
    return hcols >= 1 && hcols <= TABLE_MAX_COLS && dcols == hcols;
}

/* Lay out and emit the buffered table. Computes per-column widths from
 * every row; if the natural width fits wrap_width (or wrap_width <= 0,
 * i.e. unlimited) it renders an aligned grid with dim `│` separators,
 * otherwise it reflows each row to a bulleted record of `label: value`
 * lines (one per column) — a narrow fallback that degrades to any width. */
static void finalize_table(struct md_renderer *m)
{
    struct buf *tb = &m->table_buf;
    struct md_wrap_context ctx = wrap_context(m);
    md_wrap_commit_pending(&m->wrap, &ctx);

    /* Gather line spans (newline-terminated; table_buf always ends \n). */
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
        emit_text(m, tb->data, tb->len);
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
        emit_text(m, tb->data, tb->len);
        goto done;
    }

    /* split_row() splits on every `|`, with no awareness of inline code
     * spans or escaped `\|`, so a body cell carrying a literal pipe (e.g.
     * `` `ls | wc` `` or `a \| b`) over-splits into extra cells. GFM drops
     * the excess, but since we *transform* the table rather than pass it
     * through, dropping a cell would turn the model's literal text into
     * corrupted output. When any body row yields more cells than the
     * header, bail to verbatim so the whole block survives intact (same
     * lossless fallback used for column-count mismatch and malformed
     * rows). Fewer cells than the header is fine — those pad with empty,
     * no data loss. */
    for (int r = 2; r < lc; r++) {
        const char *bcp[TABLE_MAX_COLS];
        size_t bcl[TABLE_MAX_COLS];
        if (split_row(ls[r], ll[r], bcp, bcl, TABLE_MAX_COLS) > ncols) {
            emit_text(m, tb->data, tb->len);
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

    /* Render header cells (bold_base=1: they're rendered bold, so suppress
     * their own inner bold toggles). */
    for (int j = 0; j < ncols; j++)
        render_cell(&grid[j], hcp[j], hcl[j], m->styled, 1);
    /* Render body cells (split each row; pad missing — a row with extra
     * cells already bailed to verbatim above, so got <= ncols here). */
    for (int r = 0; r < nbody; r++) {
        const char *bcp[TABLE_MAX_COLS];
        size_t bcl[TABLE_MAX_COLS];
        int got = split_row(ls[2 + r], ll[2 + r], bcp, bcl, TABLE_MAX_COLS);
        for (int j = 0; j < ncols; j++) {
            struct cell *cell = &grid[(size_t)(1 + r) * ncols + j];
            if (j < got)
                render_cell(cell, bcp[j], bcl[j], m->styled, 0);
            else
                render_cell(cell, "", 0, m->styled, 0);
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

    /* Aligned grid when it fits, when width is unlimited (wrap_width <= 0),
     * or when there are no body rows to reflow into records — a header-only
     * table has nothing to turn into `label: value` lines, so render the
     * grid (overflow handled like any wide content) rather than emit
     * nothing. */
    if (md_wrap_width(&m->wrap) <= 0 || total <= md_wrap_width(&m->wrap) || nbody == 0) {
        /* Aligned grid with dim `│` column separators: header row, a
         * `─┼─` crossing underline, then body rows. */
        for (int j = 0; j < ncols; j++) {
            emit_cell(m, &grid[j], colw[j], align[j], 1, j == ncols - 1);
            if (j < ncols - 1)
                emit_col_sep(m);
        }
        temit(m, "\n", 1);
        temit_raw(m, ANSI_DIM);
        for (int j = 0; j < ncols; j++) {
            temit_glyphs(m, colw[j], GLYPH_HLINE);
            if (j < ncols - 1) {
                temit_glyphs(m, 1, GLYPH_HLINE); /* ─ under the left pad space */
                temit(m, GLYPH_CROSS, 3);        /* ┼ under the │ */
                temit_glyphs(m, 1, GLYPH_HLINE); /* ─ under the right pad space */
            }
        }
        temit_raw(m, ANSI_BOLD_OFF);
        temit(m, "\n", 1);
        for (int r = 0; r < nbody; r++) {
            for (int j = 0; j < ncols; j++) {
                emit_cell(m, &grid[(size_t)(1 + r) * ncols + j], colw[j], align[j], 0,
                          j == ncols - 1);
                if (j < ncols - 1)
                    emit_col_sep(m);
            }
            temit(m, "\n", 1);
        }
    } else {
        /* Too wide to align — reflow each row to a record of `label:
         * value` lines, one per column, with the first line carrying a
         * bullet. The remaining lines indent two cells so every label
         * aligns under the first; the bullet + hanging indent delimit
         * records without separator rules or blank lines. Emitted through
         * the wrap engine (emit_text / emit_cell_wrapped) so a long value
         * word-wraps with the 2-cell continuation indent the bullet/indent
         * establish — compute_indent_cells recognizes the `• ` marker. */
        for (int r = 0; r < nbody; r++) {
            struct cell *row = &grid[(size_t)(1 + r) * ncols];
            for (int j = 0; j < ncols; j++) {
                if (j == 0)
                    emit_bullet(m); /* dim "• " */
                else
                    emit_text(m, "  ", 2);
                open_bold(m);                   /* tracked, so a wrapped label keeps bold */
                emit_cell_wrapped(m, &grid[j]); /* header label */
                close_bold(m);
                emit_text(m, ": ", 2);
                emit_cell_wrapped(m, &row[j]);
                emit_text(m, "\n", 1);
            }
        }
    }

    for (int r = 0; r < nrows; r++)
        for (int j = 0; j < ncols; j++)
            cell_clear(&grid[(size_t)r * ncols + j]);
    free(grid);

done:
    buf_reset(tb);
    m->in_table = 0;
    /* The table wrote complete lines directly, so the next byte starts a
     * fresh wrapped row. */
    md_wrap_row_reset(&m->wrap);
}

/* In table-collect mode: consume one full line into table_buf, or
 * finalize the table when a blank / non-pipe line (or the byte cap) ends
 * it. Returns DEFER until a complete line is available, ADVANCED when a
 * row is buffered, PASS when the table ended (the terminating line is
 * left unconsumed for normal processing). */
static enum step_result step_in_table(struct md_renderer *m, struct buf *w, size_t *i)
{
    size_t nl = *i;
    while (nl < w->len && w->data[nl] != '\n')
        nl++;
    if (nl >= w->len)
        return STEP_DEFER; /* need the rest of the line */

    int blank = 1, pipe = 0;
    for (size_t k = *i; k < nl; k++) {
        char ch = w->data[k];
        if (ch != ' ' && ch != '\t' && ch != '\r')
            blank = 0;
        if (ch == '|')
            pipe = 1;
    }
    size_t line_len = nl - *i + 1; /* include \n */
    /* End the table on a blank / non-pipe line, or when appending this row
     * would push the buffer past the byte cap — checked against the
     * incoming row's size, not just the current length, so a single huge
     * row arriving whole (with its \n) still bails to verbatim rather than
     * growing the buffer and running layout on it. */
    if (blank || !pipe || m->table_buf.len + line_len > TABLE_MAX_BYTES) {
        finalize_table(m);
        m->at_line_start = 1;
        return STEP_PASS; /* re-dispatch this line normally */
    }
    buf_append(&m->table_buf, w->data + *i, line_len);
    *i = nl + 1;
    return STEP_ADVANCED;
}

/* Probe a `|`-led line at line start for a GFM table: it's a table iff
 * the following line is a delimiter row. On a match, buffer the header
 * and delimiter lines and enter collect mode. Returns DEFER if either
 * line isn't fully available yet, ADVANCED if a table began, PASS if it's
 * not a table (caller falls through to plain block handling). */
static enum step_result try_table_start(struct md_renderer *m, struct buf *w, size_t *i)
{
    size_t nl1 = *i;
    while (nl1 < w->len && w->data[nl1] != '\n')
        nl1++;
    if (nl1 >= w->len)
        return STEP_DEFER;
    size_t nl2 = nl1 + 1;
    while (nl2 < w->len && w->data[nl2] != '\n')
        nl2++;
    if (nl2 >= w->len)
        return STEP_DEFER;
    if (nl2 - *i + 1 > TABLE_MAX_BYTES) /* header + delimiter already over cap */
        return STEP_PASS;
    if (!table_header_matches_delim(w->data + *i, nl1 - *i, w->data + nl1 + 1, nl2 - (nl1 + 1)))
        return STEP_PASS;
    buf_append(&m->table_buf, w->data + *i, nl2 - *i + 1); /* header + delimiter, incl. \n */
    m->in_table = 1;
    *i = nl2 + 1;
    m->at_line_start = 1;
    return STEP_ADVANCED;
}

/* ---------- per-state byte dispatch ----------
 *
 * md_feed splits its work into four handlers, each owning one slice of
 * the state machine. Each returns one of three outcomes:
 *
 *   STEP_ADVANCED  — handler consumed bytes and advanced *i.
 *   STEP_DEFER     — not enough bytes to decide; loop breaks and the
 *                    unprocessed remainder is saved as tail.
 *   STEP_PASS      — handler didn't apply; try the next one. Models the
 *                    fall-throughs (inline-code closing on \n, line-start
 *                    with no special pattern matching).
 *
 * (enum step_result is defined above, before the table collectors.)
 */

/* Inside a code fence: pass through verbatim. The only thing we look for
 * is a closing fence at line start — backticks >= opener count, followed
 * by only optional whitespace and \n (no info string per CommonMark).
 * Up to 3 leading spaces before the backticks are allowed, so a fence
 * nested in a list item (whose lines carry the item's indent) still
 * closes; without this the closer reads as content and the dim region
 * runs away to end-of-stream. The marker line is consumed entirely so
 * the dim region surrounds only the content lines. */
static enum step_result step_in_code_fence(struct md_renderer *m, struct buf *w, size_t *i)
{
    char c = w->data[*i];

    if (m->at_line_start) {
        size_t scan = *i;
        size_t sp = 0;
        while (scan < w->len && sp < 3 && w->data[scan] == ' ') {
            scan++;
            sp++;
        }
        if (scan >= w->len)
            return STEP_DEFER; /* leading spaces with nothing after yet */
        if (w->data[scan] == '`') {
            size_t cnt = 0;
            while (scan + cnt < w->len && w->data[scan + cnt] == '`')
                cnt++;
            if (scan + cnt >= w->len)
                return STEP_DEFER;
            if (cnt >= m->fence_open_count) {
                /* Skip optional trailing whitespace; \r is included
                 * so a CRLF-terminated closer (`` ```\r\n ``) is
                 * recognized — without it the scan would stop at \r
                 * and treat the line as content. */
                size_t s2 = scan + cnt;
                while (s2 < w->len && w->data[s2] != '\n' &&
                       (w->data[s2] == ' ' || w->data[s2] == '\t' || w->data[s2] == '\r'))
                    s2++;
                if (s2 >= w->len)
                    return STEP_DEFER;
                if (w->data[s2] == '\n') {
                    close_code_fence(m);
                    m->fence_open_count = 0;
                    *i = s2 + 1;
                    m->at_line_start = 1;
                    return STEP_ADVANCED;
                }
                /* Non-whitespace before \n — info-string-like content
                 * (e.g. ```python inside a markdown demo). Fall through. */
            }
            /* Too few backticks or invalid trailing — fall through. */
        }
        /* Leading spaces that don't front a closer: fall through and
         * emit them verbatim as code indentation. */
    }
    emit_text(m, &c, 1);
    m->at_line_start = (c == '\n');
    (*i)++;
    return STEP_ADVANCED;
}

/* Inside an inline code span: verbatim until the closing backtick. A \n
 * inside an inline code span shouldn't happen in our prose, but if it
 * does we close the span and PASS so the inline handler emits the \n
 * with normal at_line_start bookkeeping. */
static enum step_result step_in_inline_code(struct md_renderer *m, struct buf *w, size_t *i)
{
    char c = w->data[*i];
    if (c == '`') {
        close_inline_code(m);
        (*i)++;
        return STEP_ADVANCED;
    }
    if (c == '\n') {
        close_inline_code(m);
        return STEP_PASS;
    }
    emit_text(m, &c, 1);
    (*i)++;
    return STEP_ADVANCED;
}

/* At line start: try the line-level patterns (fence open, heading, list
 * marker). Returns PASS for anything that doesn't match — the caller
 * clears at_line_start and falls through to inline processing. */
static enum step_result step_line_start(struct md_renderer *m, struct buf *w, size_t *i)
{
    /* CommonMark equivalence: up to 3 leading spaces before a block
     * marker that doesn't carry visual nesting (#, fence, thematic
     * break, blockquote, table) are equivalent to no indent. Skip
     * them before dispatch so a line like `  ## h` at the actual
     * start of a response (or after a blank line) gets the same
     * treatment as the soft-break path. List bullets (* / - / +)
     * and numbered lists are intentionally excluded — leading spaces
     * there mean nesting and must be preserved.
     *
     * Whitespace-only lines (any number of spaces or tabs followed by
     * \n) are treated as blank for paragraph-break purposes — that
     * scan is unbounded, separate from the 3-space marker cap. */
    if (w->data[*i] == ' ' || w->data[*i] == '\t') {
        size_t scan = *i;
        int has_tab = 0;
        while (scan < w->len && (w->data[scan] == ' ' || w->data[scan] == '\t')) {
            if (w->data[scan] == '\t')
                has_tab = 1;
            scan++;
        }
        if (scan >= w->len)
            return STEP_DEFER;
        char peek = w->data[scan];
        size_t avail = w->len - scan;
        size_t white = scan - *i;
        int marker_eligible = !has_tab && white <= 3;
        int normalize = 0;
        if (peek == '\n') {
            normalize = 1; /* whitespace-only line — drop, fall through to blank */
        } else if (marker_eligible && peek == '#') {
            /* CommonMark heading: 1-6 # followed by space or \n.
             * `##not` is not a heading (no space after the run). */
            size_t k = scan;
            while (k < w->len && k - scan < 6 && w->data[k] == '#')
                k++;
            if (k >= w->len)
                return STEP_DEFER;
            if (w->data[k] == ' ' || w->data[k] == '\n')
                normalize = 1;
        } else if (marker_eligible && peek == '`') {
            if (avail < 3)
                return STEP_DEFER;
            if (w->data[scan + 1] == '`' && w->data[scan + 2] == '`')
                normalize = 1;
        } else if (marker_eligible && (peek == '-' || peek == '*' || peek == '_' || peek == '=')) {
            /* Thematic break / setext underline. Allow whitespace
             * between markers and require a \n terminator — same
             * shape as the soft-break check in step_inline. */
            size_t k = scan;
            size_t count = 0;
            while (k < w->len && (w->data[k] == peek || w->data[k] == ' ' || w->data[k] == '\t' ||
                                  w->data[k] == '\r')) {
                if (w->data[k] == peek)
                    count++;
                k++;
            }
            if (k >= w->len)
                return STEP_DEFER;
            if (w->data[k] == '\n' && count >= 3)
                normalize = 1;
        } else if (marker_eligible && (peek == '>' || peek == '|')) {
            normalize = 1;
        }
        if (normalize)
            *i = scan;
    }

    char c = w->data[*i];

    /* Blank line: paragraph break. Emit a single \n and stay at_line_start
     * so a run of blank lines all hard-break (vs. the soft-join logic in
     * step_inline that would otherwise see the second \n in a "\n\n" run as
     * mid-paragraph). Consecutive blank lines collapse to one: at_blank
     * gates the emit, so a model padding with several blank lines still
     * renders a single-line gap. The paragraph-terminating \n was already
     * emitted elsewhere (step_inline hard break / marker line end), so the
     * first blank here is the second newline that opens the one gap. */
    if (c == '\n') {
        if (!m->at_blank) {
            emit_text(m, "\n", 1);
            m->at_blank = 1;
        }
        m->at_line_start = 1;
        m->cur_line_is_block = 0;
        (*i)++;
        return STEP_ADVANCED;
    }
    /* A non-blank line begins: clear the boundary so its own trailing
     * blank line (if any) is the next one allowed through. */
    m->at_blank = 0;

    if (c == '`') {
        /* Count run length so we can support 4+ backtick fences (used
         * to wrap content that itself contains ``` runs). */
        size_t cnt = 0;
        while (*i + cnt < w->len && w->data[*i + cnt] == '`')
            cnt++;
        if (*i + cnt >= w->len)
            return STEP_DEFER;
        if (cnt >= 3) {
            /* Need the rest of the opener line for the info
             * string. If \n isn't here yet, defer. */
            size_t scan = *i + cnt;
            while (scan < w->len && w->data[scan] != '\n')
                scan++;
            if (scan >= w->len)
                return STEP_DEFER;
            open_code_fence(m);
            m->fence_open_count = cnt;
            *i = scan + 1; /* past the opener line's \n */
            m->at_line_start = 1;
            return STEP_ADVANCED;
        }
        /* cnt < 3 — not a fence; fall through. */
    }

    if (c == '#') {
        size_t h = 0;
        while (h < 4 && *i + h < w->len && w->data[*i + h] == '#')
            h++;
        if (h < 4) {
            if (*i + h >= w->len)
                return STEP_DEFER;
            if (w->data[*i + h] == ' ' && h >= 1 && h <= 3) {
                open_heading(m);
                *i += h + 1;
                m->at_line_start = 0;
                return STEP_ADVANCED;
            }
        }
        /* 4+ hashes or non-heading sequence — fall through. */
    }

    /* Thematic break / setext heading underline: 3+ of `-`, `*`,
     * `_`, or `=` on a line (possibly with whitespace between),
     * terminated by \n. We consume the whole marker line verbatim
     * so step_inline doesn't misinterpret the `**` / `__` runs as
     * inline emphasis openers. CommonMark uses `=` and `-` for
     * setext h1 / h2 underlines — we don't render setext specially,
     * but the same pass-through is the right behaviour. */
    if (c == '-' || c == '*' || c == '_' || c == '=') {
        size_t k = *i;
        size_t count = 0;
        while (k < w->len &&
               (w->data[k] == c || w->data[k] == ' ' || w->data[k] == '\t' || w->data[k] == '\r')) {
            if (w->data[k] == c)
                count++;
            k++;
        }
        if (k >= w->len)
            return STEP_DEFER;
        if (w->data[k] == '\n' && count >= 3) {
            /* Thematic break (`-`/`*`/`_`) renders as a dim divider; `=` is
             * only ever a setext underline, so it's kept verbatim. */
            if (c != '=')
                render_hrule(m);
            else
                emit_text(m, w->data + *i, k - *i + 1); /* include the \n */
            *i = k + 1;
            m->at_line_start = 1;
            return STEP_ADVANCED;
        }
        /* Not a thematic break — fall through. */
    }

    /* Unordered list bullet: optional indent then a single `*`, `-`, or
     * `+` followed by a space. Render as a dim "• " — one bullet glyph for
     * all three markers (interchangeable in Markdown), with the marker
     * recessed so the item text leads. Leading indent is preserved so
     * nested lists keep depth; compute_indent_cells recognizes "• " for
     * continuation-row hanging indent. Thematic breaks (3+ markers) were
     * already handled above, so a lone marker + space here is a bullet. */
    {
        size_t k = *i;
        while (k < w->len && w->data[k] == ' ')
            k++;
        if (k < w->len && (w->data[k] == '*' || w->data[k] == '-' || w->data[k] == '+')) {
            if (k + 1 >= w->len)
                return STEP_DEFER;
            if (w->data[k + 1] == ' ') {
                /* Collapse the whole space run after the marker to the
                 * bullet's own single space (some models pad it for
                 * alignment). If the run reaches the buffer end the padding
                 * may continue in the next feed — skip_pad carries the
                 * collapse across the boundary (see step_inline) rather than
                 * deferring the whole marker. */
                size_t sp = k + 1;
                while (sp < w->len && w->data[sp] == ' ')
                    sp++;
                if (k > *i)
                    emit_text(m, w->data + *i, k - *i); /* preserve indent */
                emit_bullet(m);                         /* dim "• " */
                m->at_line_start = 0;
                m->skip_pad = (sp >= w->len);
                *i = sp; /* consume marker + space run */
                return STEP_ADVANCED;
            }
        }
    }

    /* Ordered list marker: optional indent then digits then `.`/`)` then a
     * space. Render the marker dim (number preserved) so it recedes like
     * the bullet; the item text follows via inline processing. */
    {
        size_t k = *i;
        while (k < w->len && w->data[k] == ' ')
            k++;
        if (k < w->len && w->data[k] >= '0' && w->data[k] <= '9') {
            size_t d = k;
            while (d < w->len && w->data[d] >= '0' && w->data[d] <= '9')
                d++;
            if (d >= w->len || d + 1 >= w->len)
                return STEP_DEFER;
            if ((w->data[d] == '.' || w->data[d] == ')') && w->data[d + 1] == ' ') {
                /* Collapse the whole space run after the delimiter to the
                 * single separating space below (some models pad it for
                 * alignment). If the run reaches the buffer end the padding
                 * may continue in the next feed — skip_pad carries the
                 * collapse across the boundary (see step_inline) rather than
                 * deferring the whole marker. */
                size_t sp = d + 1;
                while (sp < w->len && w->data[sp] == ' ')
                    sp++;
                if (k > *i)
                    emit_text(m, w->data + *i, k - *i); /* preserve indent */
                emit_raw(m, ANSI_DIM);
                emit_text(m, w->data + k, (d - k) + 1); /* digits + `.`/`)` */
                emit_text(m, " ", 1);                   /* separating space */
                emit_raw(m, ANSI_BOLD_OFF);             /* SGR 22 closes dim */
                m->at_line_start = 0;
                m->skip_pad = (sp >= w->len);
                *i = sp; /* consume delimiter + space run */
                return STEP_ADVANCED;
            }
        }
    }

    /* GFM table: a `|`-led line heading a delimiter row diverts into the
     * table buffer for whole-block layout. Not-a-table falls through to
     * the block-isolation handling.
     *
     * Intentionally pipe-LED only: GFM also allows borderless tables
     * (`A | B` / `---|---`), but detecting those would mean probing every
     * line that merely contains a `|` and deferring it one line to peek at
     * the delimiter — a visible streaming stall for ordinary prose with a
     * mid-line pipe (shell pipes, "a | b"). Models virtually always emit
     * leading-pipe tables, so we keep the cheap, stall-free heuristic. */
    if (c == '|') {
        enum step_result r = try_table_start(m, w, i);
        if (r == STEP_DEFER || r == STEP_ADVANCED)
            return r;
    }

    /* Blockquote `>` and GFM table row `|`: not rendered specially
     * (the marker and the row content pass through as plain text),
     * but the line is flagged as block-isolated so its trailing \n
     * is forced to a hard break — keeps a `>` line from soft-joining
     * with the prose that follows it. */
    if (c == '>' || c == '|') {
        m->cur_line_is_block = 1;
        return STEP_PASS;
    }

    return STEP_PASS;
}

/* Helper: emphasis marker is left-flanking (can open) when the byte to
 * the left is non-alphanumeric AND the byte to the right is non-space.
 * Catches the two common false positives — intraword markers (`5*3*7`,
 * `compile_commands.json`) and whitespace-flanked markers (`5 * 3`,
 * indented `  * item` list markers) — without paying for full CommonMark
 * delimiter-run rules. */
static int can_open_emphasis(char left, char right)
{
    return !is_alnum(left) && !is_space(right);
}

/* Inline byte dispatch: \n closes any open per-line styles and resets
 * at_line_start; ` opens inline code; *, **, _ open or close emphasis
 * with the left/right-flanking heuristic. Any other byte emits as text. */
static enum step_result step_inline(struct md_renderer *m, struct buf *w, size_t *i)
{
    char c = w->data[*i];
    size_t remaining = w->len - *i;

    /* Residual list-marker padding: a preceding marker consumed a space run
     * that reached the buffer end, so leading spaces at the head of this
     * feed are the continuation of that padding — drop them so the marker
     * still collapses to its single canonical space. Cleared by the first
     * non-space byte, which is the item's real content. */
    if (m->skip_pad) {
        if (c == ' ') {
            (*i)++;
            return STEP_ADVANCED;
        }
        m->skip_pad = 0;
    }

    if (c == '\n') {
        /* Headings are single-line: the trailing \n always terminates. */
        if (m->in_heading) {
            close_heading(m);
            emit_text(m, "\n", 1);
            m->at_line_start = 1;
            m->cur_line_is_block = 0;
            (*i)++;
            return STEP_ADVANCED;
        }
        /* Soft-break decision needs lookahead at the next line's
         * start. Hard cases: blank line, or the next non-blank line
         * begins with a block-level marker. Anything else is treated
         * as soft per CommonMark — single \n inside a paragraph is a
         * space. Recognized starts:
         *   "\n"              blank line / paragraph break
         *   "* " / "- " / "+ "  bullet list
         *   "1. " / "1) "     numbered list (any digit run)
         *   "# " / "## " ...  ATX heading
         *   "```"             fence opener
         *   "|"               GFM table row
         *   "> "              blockquote
         *   "---" / "***" / "___"  thematic break (also catches
         *                      setext underlines, which we don't
         *                      render but mustn't smash together)
         * CommonMark allows up to 3 leading spaces before any of
         * these — models often emit `  - sub`. Scan past them so the
         * marker check still fires; on hard break we advance past
         * the spaces too so the next line starts cleanly at the
         * marker. A leading marker char with not-yet-known followers
         * defers so the next feed can disambiguate. */
        if (remaining < 2)
            return STEP_DEFER;
        /* Two scans, one pass: walk all whitespace (spaces + tabs) so
         * blank-line detection isn't capped by leading-space count;
         * track tab presence and white-cell count separately so the
         * block-marker check can still apply CommonMark's 3-space
         * (no tabs) limit. */
        size_t scan = *i + 1;
        int has_tab = 0;
        while (scan < w->len && (w->data[scan] == ' ' || w->data[scan] == '\t')) {
            if (w->data[scan] == '\t')
                has_tab = 1;
            scan++;
        }
        if (scan >= w->len)
            return STEP_DEFER;
        char next = w->data[scan];
        size_t avail = w->len - scan;
        size_t white = scan - (*i + 1);
        int marker_eligible = !has_tab && white <= 3;

        /* Two flags drive the hard-break decision and what to do
         * about leading spaces:
         *
         *   hard            — emit \n and end the paragraph here
         *   normalize_indent — also drop the 1-3 leading spaces
         *                      (CommonMark equivalence for top-level
         *                      block markers that have no nesting
         *                      concept via indent: heading, fence,
         *                      thematic break, blockquote, table,
         *                      blank line)
         *
         * For list markers we DON'T normalize: `* parent\n  - child`
         * is a nested list and the visual indent is meaningful.
         * Same for the cur_line_is_block continuation case (a line
         * after a blockquote/table whose own next line might be
         * indented prose continuing the structure). */
        int hard = 0;
        int normalize_indent = 0;
        if (next == '\n') {
            /* Blank line (possibly with tabs / 4+ spaces) — drop the
             * whitespace and let the next iteration's blank-line
             * handler emit the second \n. */
            hard = 1;
            normalize_indent = 1;
        }
        if (!hard && marker_eligible && (next == '*' || next == '-' || next == '+')) {
            if (avail < 2)
                return STEP_DEFER;
            if (w->data[scan + 1] == ' ')
                hard = 1; /* bullet — preserve nesting indent */
        }
        /* Thematic break / setext heading underline: 3+ of the same
         * `-`, `*`, `_`, or `=` on a line, possibly with whitespace
         * between markers (CommonMark allows `* * *`, `_ _ _`, etc.).
         * `=` and `-` also serve as setext h1 / h2 underlines — we
         * don't render setext specially, but the line must stay
         * separate from prose above and below. We require the line
         * to terminate with \n so a 3-marker run mid-paragraph
         * (e.g. `intro\n--verbose ...`) doesn't false-positive. */
        if (!hard && marker_eligible &&
            (next == '-' || next == '*' || next == '_' || next == '=')) {
            size_t k = scan;
            size_t count = 0;
            while (k < w->len && (w->data[k] == next || w->data[k] == ' ' || w->data[k] == '\t' ||
                                  w->data[k] == '\r')) {
                if (w->data[k] == next)
                    count++;
                k++;
            }
            if (k >= w->len)
                return STEP_DEFER;
            if (w->data[k] == '\n' && count >= 3) {
                hard = 1;
                normalize_indent = 1;
            }
        }
        if (!hard && marker_eligible && next == '#') {
            /* CommonMark heading: 1-6 # followed by space or \n.
             * `##not` is not a heading (no space terminating the
             * hash run). */
            size_t k = scan;
            while (k < w->len && k - scan < 6 && w->data[k] == '#')
                k++;
            if (k >= w->len)
                return STEP_DEFER;
            if (w->data[k] == ' ' || w->data[k] == '\n') {
                hard = 1;
                normalize_indent = 1;
            }
        }
        if (!hard && marker_eligible && next >= '0' && next <= '9') {
            size_t k = scan + 1;
            while (k < w->len && w->data[k] >= '0' && w->data[k] <= '9')
                k++;
            if (k >= w->len || k + 1 >= w->len)
                return STEP_DEFER;
            /* `N.` and `N)` are both valid CommonMark numbered list
             * delimiters. compute_indent_cells accepts both too.
             * Numbered lists nest the same way bullets do — preserve
             * leading spaces. */
            if ((w->data[k] == '.' || w->data[k] == ')') && w->data[k + 1] == ' ')
                hard = 1;
        }
        if (!hard && marker_eligible && next == '`') {
            if (avail < 3)
                return STEP_DEFER;
            if (w->data[scan + 1] == '`' && w->data[scan + 2] == '`') {
                hard = 1;
                normalize_indent = 1;
            }
        }
        if (!hard && marker_eligible && next == '>') {
            /* Blockquotes don't nest via space-indent; nesting is
             * `> > > ...`. Leading spaces are CommonMark equivalent. */
            hard = 1;
            normalize_indent = 1;
        }
        if (!hard && marker_eligible && next == '|') {
            /* Tables don't nest. */
            hard = 1;
            normalize_indent = 1;
        }
        if (!hard && m->cur_line_is_block) {
            /* The line we're ending was a blockquote/table row. The
             * NEXT line may be lazy-continuation prose with its own
             * indent — preserve it. */
            hard = 1;
        }
        if (hard) {
            if (m->in_bold)
                close_bold(m);
            if (m->in_italic)
                close_italic(m);
            emit_text(m, "\n", 1);
            m->at_line_start = 1;
            m->cur_line_is_block = 0;
            *i = normalize_indent ? scan : *i + 1;
            return STEP_ADVANCED;
        }
        /* CommonMark hard line break: two or more spaces before the
         * newline. Checked only after the block-marker lookahead above
         * has ruled out a block opener — a hard *line* break is inline,
         * so when the next line actually starts a new block the block
         * path wins (and closes emphasis); only mid-paragraph does the
         * trailing pair force a break. The spaces were emitted eagerly
         * but, now at end-of-line before the \n, are invisible — the
         * prior bug was a soft join leaving them inline as a stray
         * double space. Open emphasis carries across (the break is
         * inline), and the next line's leading whitespace is preserved
         * so a list item's hanging continuation stays aligned. The count
         * lives in m->trailing_spaces (maintained by emit_text, reset by
         * any consumed inline delimiter) rather than a buffer scan, so it
         * holds across feeds yet ignores spaces before a delimiter. */
        if (m->trailing_spaces >= 2) {
            emit_text(m, "\n", 1);
            m->at_line_start = 1;
            m->cur_line_is_block = 0;
            (*i)++;
            return STEP_ADVANCED;
        }
        /* Soft join: replace \n with a single space and skip leading
         * whitespace on the joined line so trailing/leading spaces
         * don't double up. Styles continue across the join — the model
         * intended a paragraph that happened to be hard-wrapped, and
         * resuming bold/italic in the middle of a logical sentence is
         * what the reader expects. */
        size_t k = *i + 1;
        while (k < w->len && (w->data[k] == ' ' || w->data[k] == '\t'))
            k++;
        if (k >= w->len)
            return STEP_DEFER;
        char prev = *i > 0 ? w->data[*i - 1] : m->prev_byte;
        if (prev != ' ' && prev != '\t')
            emit_text(m, " ", 1);
        *i = k;
        return STEP_ADVANCED;
    }

    if (c == '`') {
        open_inline_code(m);
        (*i)++;
        return STEP_ADVANCED;
    }

    if (c == '*') {
        if (remaining >= 2 && w->data[*i + 1] == '*') {
            if (m->in_bold) {
                close_bold(m);
                *i += 2;
                return STEP_ADVANCED;
            }
            /* Need the byte after `**` to check the right side. */
            if (remaining < 3)
                return STEP_DEFER;
            char l = *i > 0 ? w->data[*i - 1] : m->prev_byte;
            char r = w->data[*i + 2];
            if (can_open_emphasis(l, r)) {
                open_bold(m);
                *i += 2;
                return STEP_ADVANCED;
            }
            emit_text(m, "**", 2);
            *i += 2;
            return STEP_ADVANCED;
        }
        if (remaining < 2)
            return STEP_DEFER;
        if (m->in_italic) {
            close_italic(m);
            (*i)++;
            return STEP_ADVANCED;
        }
        char l = *i > 0 ? w->data[*i - 1] : m->prev_byte;
        char r = w->data[*i + 1];
        if (can_open_emphasis(l, r)) {
            open_italic(m);
            (*i)++;
            return STEP_ADVANCED;
        }
        emit_text(m, &c, 1);
        (*i)++;
        return STEP_ADVANCED;
    }

    if (c == '_') {
        if (m->in_italic) {
            close_italic(m);
            (*i)++;
            return STEP_ADVANCED;
        }
        if (remaining < 2)
            return STEP_DEFER;
        char l = *i > 0 ? w->data[*i - 1] : m->prev_byte;
        char r = w->data[*i + 1];
        if (can_open_emphasis(l, r)) {
            open_italic(m);
            (*i)++;
            return STEP_ADVANCED;
        }
        emit_text(m, &c, 1);
        (*i)++;
        return STEP_ADVANCED;
    }

    /* Plain run: scan forward to the next marker byte (or end-of-buffer)
     * and emit the whole run in one call. Coalescing keeps the wrap
     * layer's per-codepoint bookkeeping at word granularity instead of
     * byte granularity, and halves the emit_text call count on prose. */
    size_t end = *i + 1;
    while (end < w->len) {
        char d = w->data[end];
        if (d == '\n' || d == '`' || d == '*' || d == '_')
            break;
        end++;
    }
    emit_text(m, w->data + *i, end - *i);
    *i = end;
    return STEP_ADVANCED;
}

struct md_renderer *md_new(md_emit_fn emit_cb, void *user, int wrap_width)
{
    struct md_renderer *m = xcalloc(1, sizeof(*m));
    m->emit_cb = emit_cb;
    m->user = user;
    m->at_line_start = 1;
    m->at_blank = 1; /* start of stream: swallow leading blank lines */
    m->skip_pad = 0;
    m->styled = 1;
    md_wrap_reset(&m->wrap, wrap_width);
    return m;
}

void md_reset(struct md_renderer *m, int wrap_width)
{
    buf_reset(&m->tail);
    m->prev_byte = 0;
    m->at_line_start = 1;
    m->trailing_spaces = 0;
    m->fence_open_count = 0;
    m->in_heading = 0;
    m->in_code_fence = 0;
    m->in_inline_code = 0;
    m->in_bold = 0;
    m->in_italic = 0;
    m->styled = 1;
    m->cur_line_is_block = 0;
    /* Start of stream is a blank boundary — swallow leading blank lines. */
    m->at_blank = 1;
    m->skip_pad = 0;
    /* Re-sample wrap width on turn boundary so SIGWINCH-style resizes
     * between turns are picked up cheaply without callback plumbing. */
    md_wrap_reset(&m->wrap, wrap_width);
    m->in_table = 0;
    buf_reset(&m->table_buf);
    m->inline_only = 0;
    m->suppress_bold = 0;
}

void md_free(struct md_renderer *m)
{
    if (!m)
        return;
    buf_free(&m->tail);
    md_wrap_free(&m->wrap);
    buf_free(&m->table_buf);
    free(m);
}

void md_feed(struct md_renderer *m, const char *s, size_t n)
{
    /* Combine pending tail with new input into a working buffer so the
     * walker sees one contiguous stream. */
    struct buf w;
    buf_init(&w);
    if (m->tail.len) {
        buf_append(&w, m->tail.data, m->tail.len);
        buf_reset(&m->tail);
    }
    buf_append(&w, s, n);

    size_t i = 0;
    while (i < w.len) {
        enum step_result step;

        if (m->in_table) {
            step = step_in_table(m, &w, &i);
            if (step == STEP_DEFER)
                break;
            if (step == STEP_ADVANCED)
                continue;
            /* STEP_PASS — table ended; fall through to handle this line. */
        }

        if (m->in_code_fence) {
            step = step_in_code_fence(m, &w, &i);
            if (step == STEP_DEFER)
                break;
            continue;
        }

        if (m->in_inline_code) {
            step = step_in_inline_code(m, &w, &i);
            if (step == STEP_DEFER)
                break;
            if (step == STEP_ADVANCED)
                continue;
            /* STEP_PASS — \n inside inline code; let inline
             * processing handle the actual newline emit. */
        }

        if (m->at_line_start && !m->inline_only) {
            step = step_line_start(m, &w, &i);
            if (step == STEP_DEFER)
                break;
            if (step == STEP_ADVANCED)
                continue;
            /* STEP_PASS — no line-start pattern matched; clear
             * at_line_start and fall through to inline. */
            m->at_line_start = 0;
        }

        step = step_inline(m, &w, &i);
        if (step == STEP_DEFER)
            break;
        /* step_inline always advances or defers — never PASSes. */
    }

    /* Save unprocessed remainder as tail. The buf grows dynamically so a
     * long fence opener line that spans deltas is buffered intact. The
     * sanity cap prevents runaway growth on malformed input — the leading
     * excess is flushed as plain text rather than held forever.
     *
     * Skip the excess-flush while collecting a table: the deferred bytes
     * are an incomplete table row awaiting its newline, not an ambiguous
     * marker tail. Flushing the leading part as prose would leak text
     * ahead of the rendered table and split the row (step_in_table appends
     * whole lines to table_buf, and TABLE_MAX_BYTES already bounds it). */
    size_t rem = w.len - i;
    if (m->in_table && rem > TABLE_MAX_BYTES) {
        /* In-progress table row (no newline yet) past the byte cap —
         * malformed/pathological. The excess-flush above is suppressed
         * while in_table, so without this the tail would grow unbounded.
         * Bail to verbatim: dump the buffered rows and the oversized
         * partial as plain text, and leave table mode. */
        emit_text(m, m->table_buf.data, m->table_buf.len);
        buf_reset(&m->table_buf);
        m->in_table = 0;
        emit_text(m, w.data + i, rem);
        i = w.len;
        rem = 0;
    }
    if (rem > TAIL_MAX && !m->in_table) {
        emit_text(m, w.data + i, rem - TAIL_MAX);
        i += rem - TAIL_MAX;
        rem = TAIL_MAX;
    }
    if (rem > 0)
        buf_append(&m->tail, w.data + i, rem);

    /* Remember the last source byte we consumed so the next feed's
     * intraword check at i=0 sees the correct left neighbor. */
    if (i > 0)
        m->prev_byte = w.data[i - 1];

    buf_free(&w);
}

void md_flush(struct md_renderer *m)
{
    /* End-of-stream interpretation of any pending tail: the streaming loop
     * deferred these bytes because it needed lookahead to decide. At flush
     * the lookahead is "end of stream", which counts as non-alphanumeric
     * for marker validity — so a trailing closer like ``` (no newline),
     * `*` after italic, or `**` after bold should match cleanly instead of
     * being emitted as literal text. */

    /* A header + delimiter whose delimiter line never got a trailing \n:
     * try_table_start deferred (it needs the second newline) so in_table
     * is still false and the tail holds the whole "header\ndelimiter". At
     * EOF the bytes-we-have ARE both lines, so recognize the table now and
     * render it the same as if it had a final newline. Pipe-led only, to
     * match the streaming detection scope. */
    if (!m->in_table && m->tail.len > 0 && m->tail.data[0] == '|') {
        size_t nl1 = 0;
        while (nl1 < m->tail.len && m->tail.data[nl1] != '\n')
            nl1++;
        if (nl1 < m->tail.len &&                  /* header line complete */
            m->tail.len + 1 <= TABLE_MAX_BYTES && /* header + delimiter within cap */
            table_header_matches_delim(m->tail.data, nl1, m->tail.data + nl1 + 1,
                                       m->tail.len - (nl1 + 1))) {
            buf_append(&m->table_buf, m->tail.data, m->tail.len);
            buf_append(&m->table_buf, "\n", 1);
            buf_reset(&m->tail);
            m->in_table = 1;
        }
    }

    /* A table still collecting at end-of-stream: absorb a trailing
     * newline-less last row from the tail (deferred for its missing \n),
     * then lay out what we have. The combined-size check mirrors the other
     * three entry points (step_in_table, try_table_start, the header probe
     * above): if appending this final row would push table_buf past the
     * byte cap, leave the tail untouched — finalize what's within the cap,
     * and the leftover row falls through to the literal-emit fallback
     * below, exactly as step_in_table bails an over-cap completed row. */
    if (m->in_table) {
        int blank = 1, pipe = 0;
        for (size_t k = 0; k < m->tail.len; k++) {
            char ch = m->tail.data[k];
            if (ch != ' ' && ch != '\t' && ch != '\r')
                blank = 0;
            if (ch == '|')
                pipe = 1;
        }
        if (m->tail.len > 0 && !blank && pipe &&
            m->table_buf.len + m->tail.len + 1 <= TABLE_MAX_BYTES) {
            buf_append(&m->table_buf, m->tail.data, m->tail.len);
            buf_append(&m->table_buf, "\n", 1);
            buf_reset(&m->tail);
        }
        finalize_table(m);
    }

    if (m->in_code_fence && m->tail.len > 0) {
        /* Skip up to 3 leading spaces, matching step_in_code_fence —
         * an indented closer (`  ``` ` nested in a list item) left in
         * the tail at EOF must still be recognized, not emitted as
         * literal/dim content. */
        size_t lead = 0;
        while (lead < m->tail.len && lead < 3 && m->tail.data[lead] == ' ')
            lead++;
        size_t cnt = 0;
        while (lead + cnt < m->tail.len && m->tail.data[lead + cnt] == '`')
            cnt++;
        if (cnt >= m->fence_open_count) {
            int valid = 1;
            for (size_t i = lead + cnt; i < m->tail.len; i++) {
                char tc = m->tail.data[i];
                if (tc != ' ' && tc != '\t' && tc != '\r') {
                    valid = 0;
                    break;
                }
            }
            if (valid) {
                close_code_fence(m);
                m->fence_open_count = 0;
                buf_reset(&m->tail);
            }
        }
    }

    if (m->tail.len == 2 && m->in_bold && m->tail.data[0] == '*' && m->tail.data[1] == '*') {
        close_bold(m);
        buf_reset(&m->tail);
    }

    if (m->tail.len == 1 && m->in_italic && (m->tail.data[0] == '*' || m->tail.data[0] == '_')) {
        close_italic(m);
        buf_reset(&m->tail);
    }

    /* End-of-stream resolution for a deferred soft-break decision.
     * The streaming \n handler defers when it can't tell yet whether
     * the next line starts with a block marker (`**`, `## `, `1. `,
     * `1) `, ` ``` `, etc. — any prefix needing more bytes for
     * disambiguation). At md_flush we know no more bytes are coming,
     * so an unmatched prefix is by definition NOT a marker — resolve
     * the deferred \n as a soft join: emit a single space (unless the
     * previous line already ended in whitespace) and pass the rest
     * of the tail through as content. The blank-line case (tail is
     * \n + only whitespace) stays a hard break.
     *
     * This only applies when the tail starts with \n; the marker-
     * specific branches above (fence closer, ` ** ` / ` * ` / ` _ `
     * tails) already handled their own EOF resolution. */
    if (m->tail.len > 0 && m->tail.data[0] == '\n') {
        size_t k = 1;
        while (k < m->tail.len && (m->tail.data[k] == ' ' || m->tail.data[k] == '\t'))
            k++;
        if (k >= m->tail.len) {
            /* "\n" alone, or "\n" + only whitespace — hard break. */
            emit_text(m, "\n", 1);
            buf_reset(&m->tail);
        } else if (m->tail.data[k] == '\n') {
            /* "\n  \n..." — paragraph break followed by more content.
             * Emit the first \n; let the rest fall through as plain
             * text below (the leading whitespace + second \n + tail). */
            emit_text(m, "\n", 1);
            /* Drop the leading whitespace before the second \n
             * (CommonMark normalization), then keep the remainder. */
            size_t rest_off = k;
            size_t rest_len = m->tail.len - rest_off;
            char *rest = xmalloc(rest_len);
            memcpy(rest, m->tail.data + rest_off, rest_len);
            buf_reset(&m->tail);
            emit_text(m, rest, rest_len);
            free(rest);
        } else {
            /* Before falling back to soft-join, check if the tail
             * contains a thematic break or setext underline that
             * happens to lack a trailing \n at end-of-stream. The
             * streaming check requires \n to disambiguate; at EOF
             * the bytes-we-have ARE the whole line, so a 3+ run of
             * `-` / `*` / `_` / `=` (with whitespace allowed
             * between) is a valid hard break even without \n.
             * Heading / list / blockquote / table can't reach this
             * branch — those marker checks never defer once the
             * required bytes are present, so they were resolved by
             * the streaming path. */
            char first = m->tail.data[k];
            int hard_at_eof = 0;
            if (first == '-' || first == '*' || first == '_' || first == '=') {
                size_t j = k;
                size_t count = 0;
                while (j < m->tail.len && (m->tail.data[j] == first || m->tail.data[j] == ' ' ||
                                           m->tail.data[j] == '\t' || m->tail.data[j] == '\r')) {
                    if (m->tail.data[j] == first)
                        count++;
                    j++;
                }
                if (j >= m->tail.len && count >= 3)
                    hard_at_eof = 1;
            }
            if (hard_at_eof) {
                /* Hard break, then the marker line. A `-`/`*`/`_` thematic
                 * break renders as the dim divider (same as the streaming
                 * path); a `=` setext underline stays literal. Leading
                 * whitespace before the marker is dropped per CommonMark. */
                emit_text(m, "\n", 1);
                if (eof_thematic_marker(m->tail.data, k, m->tail.len)) {
                    render_hrule(m);
                    buf_reset(&m->tail);
                } else {
                    size_t rest_len = m->tail.len - k;
                    char *rest = xmalloc(rest_len);
                    memcpy(rest, m->tail.data + k, rest_len);
                    buf_reset(&m->tail);
                    emit_text(m, rest, rest_len);
                    free(rest);
                }
            } else if (m->trailing_spaces >= 2) {
                /* The streaming \n handler deferred here for an ambiguous
                 * block prefix (`#`, `-`, `2024`, ...) that EOF resolves
                 * to prose. The two trailing spaces still make it a hard
                 * line break — emit \n and the rest verbatim (leading
                 * whitespace preserved, matching the streaming inline
                 * hard break) instead of soft-joining. */
                emit_text(m, "\n", 1);
                size_t rest_len = m->tail.len - 1;
                char *rest = xmalloc(rest_len);
                memcpy(rest, m->tail.data + 1, rest_len);
                buf_reset(&m->tail);
                emit_text(m, rest, rest_len);
                free(rest);
            } else {
                /* Soft join: space + rest of content (past leading ws). */
                char prev = m->prev_byte;
                if (prev != ' ' && prev != '\t' && prev != 0)
                    emit_text(m, " ", 1);
                size_t rest_len = m->tail.len - k;
                char *rest = xmalloc(rest_len);
                memcpy(rest, m->tail.data + k, rest_len);
                buf_reset(&m->tail);
                emit_text(m, rest, rest_len);
                free(rest);
            }
        }
    }

    /* A bare thematic break left in the tail at EOF (input ended with
     * `---`/`***`/`___` and never got its \n) renders as the dim divider,
     * matching the streaming path. */
    if (m->tail.len) {
        size_t s = 0;
        while (s < m->tail.len && (m->tail.data[s] == ' ' || m->tail.data[s] == '\t'))
            s++;
        if (eof_thematic_marker(m->tail.data, s, m->tail.len)) {
            render_hrule(m);
            buf_reset(&m->tail);
        }
    }

    /* Anything still in tail is a marker that never matched — emit literally. */
    if (m->tail.len) {
        emit_text(m, m->tail.data, m->tail.len);
        buf_reset(&m->tail);
    }

    /* Close any styles still open so the terminal isn't left in a
     * styled state (e.g. an unmatched `**bold` opener). */
    if (m->in_inline_code)
        close_inline_code(m);
    if (m->in_code_fence)
        close_code_fence(m);
    if (m->in_heading)
        close_heading(m);
    if (m->in_bold)
        close_bold(m);
    if (m->in_italic)
        close_italic(m);

    struct md_wrap_context ctx = wrap_context(m);
    md_wrap_flush(&m->wrap, &ctx);
}

void md_set_styled(struct md_renderer *m, int on)
{
    if (m->styled == on)
        return;
    /* Flush the tail under the OLD setting so deferred markers resolve
     * with the SGR rules they were buffered under, then soft-reset
     * (same fields as md_reset, wrap_width preserved) so the next feed
     * starts at column 0 to match the seam the caller is emitting. */
    md_flush(m);
    md_reset(m, md_wrap_width(&m->wrap));
    m->styled = on;
}

int md_in_table(const struct md_renderer *m)
{
    /* True while rows are being diverted into table_buf with nothing
     * emitted yet — the grid only renders at finalize_table once every
     * row is in. The agent reads this to surface a "composing..."
     * spinner during the otherwise-silent accumulation. */
    return m && m->in_table;
}
