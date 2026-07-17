/* SPDX-License-Identifier: MIT */
#include "render/markdown.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "render/markdown_scan.h"
#include "render/markdown_table.h"
#include "render/markdown_wrap.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"

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

    struct md_table table;

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

/* ---------- table integration ---------- */

static void table_emit_direct(const char *bytes, size_t n, int is_raw, void *user)
{
    struct md_renderer *m = user;
    m->emit_cb(bytes, n, is_raw, m->user);
}

static void table_emit_text(void *user, const char *s, size_t n)
{
    emit_text(user, s, n);
}

static void table_emit_raw(void *user, const char *s, size_t n)
{
    struct md_renderer *m = user;
    if (!m->styled || n == 0)
        return;
    struct md_wrap_context ctx = wrap_context(m);
    md_wrap_emit_raw(&m->wrap, &ctx, s, n, 0);
}

/* Cell escapes must update semantic state before the wrapper snapshots a break. */
static void table_replay_raw(void *user, const char *s, size_t n)
{
    struct md_renderer *m = user;
    size_t p = 0;
    while (p < n) {
        size_t q = p;
        while (q < n && s[q] != 'm')
            q++;
        if (q < n)
            q++;
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
    table_emit_raw(m, s, n);
}

static void table_open_bold(void *user)
{
    open_bold(user);
}

static void table_close_bold(void *user)
{
    close_bold(user);
}

static void table_render_inline(void *user, const char *s, size_t n, int bold_base,
                                md_table_emit_fn emit, void *emit_user)
{
    struct md_renderer *m = user;
    struct md_renderer *sub = md_new(emit, emit_user, 0);
    if (!m->styled)
        md_set_styled(sub, 0);
    sub->inline_only = 1;
    sub->suppress_bold = bold_base;
    md_feed(sub, s, n);
    md_flush(sub);
    md_free(sub);
}

static void table_commit_pending(void *user)
{
    struct md_renderer *m = user;
    struct md_wrap_context ctx = wrap_context(m);
    md_wrap_commit_pending(&m->wrap, &ctx);
}

static void table_row_reset(void *user)
{
    struct md_renderer *m = user;
    md_wrap_row_reset(&m->wrap);
}

static struct md_table_context table_context(struct md_renderer *m)
{
    return (struct md_table_context){
        .user = m,
        .emit_direct = table_emit_direct,
        .emit_text = table_emit_text,
        .emit_raw = table_emit_raw,
        .replay_raw = table_replay_raw,
        .open_bold = table_open_bold,
        .close_bold = table_close_bold,
        .render_inline = table_render_inline,
        .commit_pending = table_commit_pending,
        .row_reset = table_row_reset,
        .styled = m->styled,
        .wrap_width = md_wrap_width(&m->wrap),
    };
}

/* Per-state byte-dispatch outcome. STEP_ADVANCED: consumed bytes, advanced
 * *i. STEP_DEFER: not enough bytes to decide. STEP_PASS: handler didn't
 * apply; try the next. */
enum step_result {
    STEP_ADVANCED,
    STEP_DEFER,
    STEP_PASS,
};

/* ---------- thematic breaks ---------- */

/* The model's thematic-break divider is a short row of spaced dim dots (a
 * typographic "dinkus"), so it reads as a quiet content separator rather
 * than competing with the harness's own structural chrome — the dim SOLID
 * full-width `─` rules in transcript.c and the `── resumed ──` marker in
 * agent.c. The convention: solid rule = system, spaced dots = model
 * divider. Tables use the solid line family for their internal grid. */
#define HRULE_DOT_GAP 3              /* spaces between divider dots */
#define GLYPH_DOT     "\xc2\xb7"     /* · middle dot — model divider */
#define GLYPH_BULLET  "\xe2\x80\xa2" /* • list marker */

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

/* Emit a dim list bullet through the wrapper so hanging indentation sees it. */
static void emit_bullet(struct md_renderer *m)
{
    emit_raw(m, ANSI_DIM);
    emit_text(m, GLYPH_BULLET " ", 4);
    emit_raw(m, ANSI_BOLD_OFF); /* SGR 22 closes dim */
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
 */

/* Inside a code fence: pass through verbatim. The only thing we look for
 * is a closing fence at line start — backticks >= opener count, followed
 * by only optional whitespace and \n (no info string per CommonMark).
 * Up to 3 leading spaces before the backticks are allowed, so a fence
 * nested in a list item (whose lines carry the item's indent) still
 * closes; without this the closer reads as content and the dim region
 * runs away to end-of-stream. The marker line is consumed entirely so
 * the dim region surrounds only the content lines. */
static enum step_result step_in_code_fence(struct md_renderer *m, struct buf *w, size_t *i,
                                           int final)
{
    char c = w->data[*i];

    if (m->at_line_start) {
        size_t scan = *i;
        size_t sp = 0;
        while (scan < w->len && sp < 3 && w->data[scan] == ' ') {
            scan++;
            sp++;
        }
        if (scan >= w->len) {
            if (!final)
                return STEP_DEFER; /* leading spaces with nothing after yet */
        } else if (w->data[scan] == '`') {
            size_t cnt = 0;
            while (scan + cnt < w->len && w->data[scan + cnt] == '`')
                cnt++;
            if (scan + cnt >= w->len && !final)
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
                if (s2 >= w->len) {
                    if (!final)
                        return STEP_DEFER;
                    /* EOF terminates a valid closer followed only by optional whitespace. */
                    close_code_fence(m);
                    m->fence_open_count = 0;
                    *i = s2;
                    m->at_line_start = 1;
                    return STEP_ADVANCED;
                }
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
static enum step_result step_line_start(struct md_renderer *m, struct buf *w, size_t *i, int final)
{
    struct md_line_info line = md_scan_line(w->data + *i, w->len - *i, final);
    int normalize_indent = line.normalize_indent;
    /* A bare setext underline stays literal at EOF, including its source indent. */
    if (final && line.kind == MD_LINE_THEMATIC && line.marker == '=')
        normalize_indent = 0;
    if (normalize_indent)
        *i += line.indent_length;
    /* Unindented dispatchers can resolve conservative lookahead results themselves. */
    if (!line.classification_complete && line.indent_length > 0)
        return STEP_DEFER;

    if (final && line.kind == MD_LINE_TEXT) {
        /* A block prefix that remained ambiguous through EOF falls back to literal text. */
        struct md_line_info streaming = md_scan_line(w->data + *i, w->len - *i, 0);
        if (!streaming.classification_complete) {
            emit_text(m, w->data + *i, w->len - *i);
            *i = w->len;
            return STEP_ADVANCED;
        }
    }

    if (final && line.kind == MD_LINE_THEMATIC) {
        if (line.marker == '=')
            emit_text(m, w->data + *i, w->len - *i);
        else
            render_hrule(m);
        *i = w->len;
        m->at_line_start = 1;
        return STEP_ADVANCED;
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
        enum md_table_result r = md_table_try_start(&m->table, w, i);
        if (r == MD_TABLE_DEFER) {
            if (!final)
                return STEP_DEFER;
            /* md_table_finish already had the final chance; unresolved bytes stay literal. */
            emit_text(m, w->data + *i, w->len - *i);
            *i = w->len;
            return STEP_ADVANCED;
        }
        if (r == MD_TABLE_ADVANCED)
            return STEP_ADVANCED;
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
static enum step_result step_inline(struct md_renderer *m, struct buf *w, size_t *i, int final)
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
        /* A physical newline is hard before a blank or block line and soft inside prose. The
         * scanner may identify a definite block before its exact kind is complete, preserving
         * eager boundary output while line dispatch waits for more. */
        struct md_line_info line = md_scan_line(w->data + *i + 1, remaining - 1, final);
        if (line.kind == MD_LINE_INCOMPLETE) {
            /* Indented list candidates are not block starts in lookahead, but an all-whitespace
             * prefix remains ambiguous regardless of indentation depth. */
            if (line.indent_length == remaining - 1 || line.indent_length <= 3)
                return STEP_DEFER;
        }
        /* At EOF, whitespace keeps a hard break, thematic markers become complete lines, and
         * unresolved prefixes remain literal rather than entering inline parsing. */
        if (final && line.kind == MD_LINE_TEXT && line.indent_length == remaining - 1) {
            emit_text(m, "\n", 1);
            *i = w->len;
            m->at_line_start = 1;
            m->cur_line_is_block = 0;
            return STEP_ADVANCED;
        }
        if (final && line.kind == MD_LINE_THEMATIC) {
            emit_text(m, "\n", 1);
            *i += 1 + line.indent_length;
            m->at_line_start = 1;
            m->cur_line_is_block = 0;
            return STEP_ADVANCED;
        }
        if (final && line.kind == MD_LINE_TEXT) {
            if (m->trailing_spaces >= 2) {
                emit_text(m, "\n", 1);
                emit_text(m, w->data + *i + 1, remaining - 1);
            } else {
                char prev = *i > 0 ? w->data[*i - 1] : m->prev_byte;
                if (prev != ' ' && prev != '\t' && prev != 0)
                    emit_text(m, " ", 1);
                size_t scan = *i + 1 + line.indent_length;
                emit_text(m, w->data + scan, w->len - scan);
            }
            *i = w->len;
            return STEP_ADVANCED;
        }

        int hard = 0;
        if (line.kind == MD_LINE_BLANK || line.kind == MD_LINE_HEADING ||
            line.kind == MD_LINE_FENCE || line.kind == MD_LINE_THEMATIC ||
            line.kind == MD_LINE_BLOCKQUOTE || line.kind == MD_LINE_PIPE)
            hard = 1;
        if ((line.kind == MD_LINE_BULLET || line.kind == MD_LINE_ORDERED) &&
            line.indent_length <= 3)
            hard = 1;
        if (m->cur_line_is_block)
            hard = 1;

        size_t scan = *i + 1 + line.indent_length;
        int normalize_indent = line.normalize_indent;
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
            if (remaining < 3) {
                if (!final)
                    return STEP_DEFER;
                emit_text(m, "**", 2);
                *i += 2;
                return STEP_ADVANCED;
            }
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
        if (remaining < 2) {
            if (!final)
                return STEP_DEFER;
            /* A lone final star closes active italic but is otherwise literal. */
            if (m->in_italic)
                close_italic(m);
            else
                emit_text(m, &c, 1);
            (*i)++;
            return STEP_ADVANCED;
        }
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
        if (remaining < 2) {
            if (!final)
                return STEP_DEFER;
            emit_text(m, &c, 1);
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
    md_table_reset(&m->table);
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
    md_table_reset(&m->table);
    m->inline_only = 0;
    m->suppress_bold = 0;
}

void md_free(struct md_renderer *m)
{
    if (!m)
        return;
    buf_free(&m->tail);
    md_wrap_free(&m->wrap);
    md_table_free(&m->table);
    free(m);
}

/* Final mode resolves deferred prefixes against EOF instead of saving another tail. */
static void md_process(struct md_renderer *m, const char *s, size_t n, int final)
{
    /* Combine pending tail with new input into a working buffer so the
     * walker sees one contiguous stream. */
    struct buf w;
    buf_init(&w);
    if (m->tail.len) {
        buf_append(&w, m->tail.data, m->tail.len);
        buf_reset(&m->tail);
    }
    if (n)
        buf_append(&w, s, n);

    size_t i = 0;
    while (i < w.len) {
        enum step_result step;

        if (md_table_is_collecting(&m->table)) {
            struct md_table_context ctx = table_context(m);
            enum md_table_result table_step = md_table_step(&m->table, &ctx, &w, &i);
            if (table_step == MD_TABLE_DEFER)
                break;
            if (table_step == MD_TABLE_ADVANCED)
                continue;
            m->at_line_start = 1;
            /* The table ended; fall through to handle this line. */
        }

        if (m->in_code_fence) {
            step = step_in_code_fence(m, &w, &i, final);
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
            step = step_line_start(m, &w, &i, final);
            if (step == STEP_DEFER)
                break;
            if (step == STEP_ADVANCED)
                continue;
            /* STEP_PASS — no line-start pattern matched; clear
             * at_line_start and fall through to inline. */
            m->at_line_start = 0;
        }

        step = step_inline(m, &w, &i, final);
        if (step == STEP_DEFER)
            break;
        /* step_inline always advances or defers — never PASSes. */
    }

    /* Save ambiguous parser input as tail. Incomplete table rows stay whole
     * so no bytes leak ahead of their buffered table. */
    size_t rem = w.len - i;
    struct md_table_context table_ctx = table_context(m);
    if (rem > 0 && md_table_bail_partial(&m->table, &table_ctx, w.data + i, rem)) {
        i = w.len;
        rem = 0;
    }
    if (final && rem > 0) {
        /* Preserve bytes literally if a handler unexpectedly still defers at EOF. */
        emit_text(m, w.data + i, rem);
        i = w.len;
        rem = 0;
    }
    if (rem > TAIL_MAX && !md_table_is_collecting(&m->table)) {
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

void md_feed(struct md_renderer *m, const char *s, size_t n)
{
    md_process(m, s, n, 0);
}

void md_flush(struct md_renderer *m)
{
    int was_collecting_table = md_table_is_collecting(&m->table);
    struct md_table_context table_ctx = table_context(m);
    md_table_finish(&m->table, &table_ctx, &m->tail);
    /* A rejected final row is table fallback output, not fresh Markdown input. */
    if (was_collecting_table && m->tail.len > 0) {
        emit_text(m, m->tail.data, m->tail.len);
        buf_reset(&m->tail);
    }
    md_process(m, NULL, 0, 1);

    /* Close any styles still open so the terminal isn't left in a styled state. */
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
    /* The agent surfaces a spinner during otherwise-silent accumulation. */
    return m && md_table_is_collecting(&m->table);
}
