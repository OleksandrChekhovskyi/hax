/* SPDX-License-Identifier: MIT */
#include "render/markdown.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "terminal/ansi.h"
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

    /* Number of backticks in the active fence's opener — closer must have
     * at least this many, with no info string after, per CommonMark. Lets
     * a ```markdown demo containing inner ```python lines stay open until
     * a *bare* ``` line. */
    size_t fence_open_count;

    /* Style flags. All independent — each maps to a distinct SGR group, so
     * e.g. inline code (cyan) inside bold leaves bold intact when the code
     * span closes. The one exception is heading-vs-inline-bold which both
     * use the same SGR; close_bold re-emits ANSI_BOLD if in_heading is
     * still set so the rest of the heading line stays bold. */
    int in_heading;
    int in_code_fence;
    int in_inline_code;
    int in_bold;
    int in_italic;

    /* Set when the current line starts with a marker that isolates it
     * from surrounding prose (blockquote `>`, GFM table row `|`). The
     * trailing \n of such a line is forced hard regardless of what's
     * on the next line — so a blockquote line followed by plain
     * prose doesn't soft-join into a single paragraph. Cleared on
     * every hard \n; set in step_line_start when the leading byte
     * matches. */
    int cur_line_is_block;

    /* ---------- wrap layer ----------
     *
     * Active when wrap_width > 0. Sits between the markdown step machine
     * and the emit callback: content bytes go through emit_text, ANSI
     * escapes through emit_raw; both buffer into row_buf with a parallel
     * row_meta byte (0 = content, 1 = raw) so the wrap-flush can re-emit
     * runs of like-kind bytes through emit_cb with the correct is_raw
     * flag. Codepoints are reassembled by cp_stream so cell counts are
     * exact even when step_inline hands us bytes mid-sequence.
     *
     * Wrap is bypassed when in_code_fence (verbatim — terminal hard-wraps
     * if needed) and in_heading (single-line by convention). Inline code
     * spans are NOT bypassed — they wrap normally at embedded spaces; SGR
     * persistence across \n keeps the cyan run continuous across the
     * inserted break. */
    int wrap_width;
    struct utf8_stream cp_stream;
    struct buf row_buf;
    struct buf row_meta;
    /* Cells used so far in the current row. Excludes raw-escape bytes. */
    int col;
    /* Byte offset in row_buf of the most recent break-safe space, or -1
     * if no break is recorded yet on this row. Pointing AT the space —
     * wrap emits row_buf[0..break_byte] and skips the space byte itself. */
    int last_break_byte;
    /* Cell column immediately AFTER the break-space, used to recompute
     * col when content is carried into the next row. */
    int last_break_col;
    /* Continuation indent for wrapped rows. Computed lazily on the first
     * wrap of a logical line — looks at row_buf for `* `, `- `, `+ `,
     * `N. ` / `N) ` patterns and locks the indent so subsequent wraps
     * on the same logical line don't re-detect (the carried-over
     * content no longer starts with the marker). Zero outside list
     * items. Reset on every hard \n. */
    int indent_cells;
    int indent_locked;
    /* Whether any non-space content has landed on the current row. Until
     * it does, leading spaces don't count as break candidates — would
     * just produce empty leading rows on overflow. */
    int row_has_content;
};

static int is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* ---------- wrap layer ----------
 *
 * Forwarding helpers used by emit_text / emit_raw below and by the
 * step machine to flush at hard-break boundaries. When wrap_width is
 * 0 (disabled) all of these collapse to a direct emit_cb call.
 */

/* Append bytes to row_buf with their is_raw kind tagged in row_meta.
 * The two arrays stay in lockstep so wrap_flush_range can re-emit runs
 * of like-kind bytes through emit_cb with the correct flag. */
static void wrap_append(struct md_renderer *m, const char *s, size_t n, int is_raw)
{
    buf_append(&m->row_buf, s, n);
    char meta = is_raw ? 1 : 0;
    for (size_t i = 0; i < n; i++)
        buf_append(&m->row_meta, &meta, 1);
}

/* Emit row_buf[start..end), splitting into runs of identical row_meta
 * bytes so escape spans go out with is_raw=1 and content with is_raw=0.
 * The split lets disp's trail/held bookkeeping ignore zero-width bytes
 * exactly the way it does on the non-wrap path. */
static void wrap_flush_range(struct md_renderer *m, size_t start, size_t end)
{
    size_t i = start;
    while (i < end) {
        char kind = m->row_meta.data[i];
        size_t j = i + 1;
        while (j < end && m->row_meta.data[j] == kind)
            j++;
        m->emit_cb(m->row_buf.data + i, j - i, kind ? 1 : 0, m->user);
        i = j;
    }
}

static void wrap_flush_all(struct md_renderer *m)
{
    if (m->row_buf.len > 0)
        wrap_flush_range(m, 0, m->row_buf.len);
    buf_reset(&m->row_buf);
    buf_reset(&m->row_meta);
}

/* Detect a leading list-marker pattern in row_buf so wrapped rows can
 * indent under the marker's content column. Skips raw escape bytes
 * (they may be a leading style opener) and leading content spaces
 * (preserved before nested or indented list markers per the soft-break
 * decision). Recognizes:
 *   `* `, `- `, `+ `   → leading_spaces + 2 cells
 *   `N. ` / `N) `      → leading_spaces + digits + 2 cells
 * Returns 0 when no marker matches. ASCII-only patterns, so byte
 * offsets equal cell counts. The leading-space scan is capped at 8
 * to keep the continuation indent within sane budget territory; deeper
 * nesting levels just lose a bit of visual hierarchy on wrap. */
static int compute_indent_cells(const struct md_renderer *m)
{
    size_t i = 0;
    /* Skip leading raw-escape bytes — bullets are emitted as plain
     * content, so the first non-raw byte is the candidate marker. */
    while (i < m->row_buf.len && m->row_meta.data[i] == 1)
        i++;
    /* Then skip leading ASCII spaces in content. */
    size_t lead_spaces = 0;
    while (i < m->row_buf.len && lead_spaces < 8 && m->row_meta.data[i] == 0 &&
           m->row_buf.data[i] == ' ') {
        i++;
        lead_spaces++;
    }
    if (i >= m->row_buf.len)
        return 0;
    char c = m->row_buf.data[i];
    if ((c == '*' || c == '-' || c == '+') && i + 1 < m->row_buf.len &&
        m->row_meta.data[i + 1] == 0 && m->row_buf.data[i + 1] == ' ')
        return (int)lead_spaces + 2;
    if (c >= '0' && c <= '9') {
        /* Scan the full digit run — match the soft-break detector
         * which is also unbounded, so a 4+ digit ordered list like
         * "1000. " or CommonMark's 9-digit max gets a correct
         * hanging indent under the marker on continuation rows. */
        size_t j = i + 1;
        while (j < m->row_buf.len && m->row_meta.data[j] == 0 && m->row_buf.data[j] >= '0' &&
               m->row_buf.data[j] <= '9')
            j++;
        if (j > i && j + 1 < m->row_buf.len && m->row_meta.data[j] == 0 &&
            (m->row_buf.data[j] == '.' || m->row_buf.data[j] == ')') &&
            m->row_meta.data[j + 1] == 0 && m->row_buf.data[j + 1] == ' ')
            return (int)lead_spaces + (int)(j - i) + 2; /* spaces + digits + delim + space */
    }
    return 0;
}

/* Continuation budget — wrap_width on the first row, but at least
 * indent_cells + 20 on continuation rows so a deeply-indented bullet
 * still has room to make forward progress (wrap into 3 cells of
 * content would just spin). The min-content floor lets the row
 * overshoot wrap_width when indent is huge — the alternative is
 * worse: visible spinning at the per-codepoint break. */
static int current_row_budget(struct md_renderer *m)
{
    /* No indent (plain paragraph): always wrap_width. With indent
     * (continuation row of a list item), guarantee at least 20 cells
     * of content past the indent so a deeply-indented bullet still
     * makes forward progress — overshoots wrap_width in that edge
     * case, which is the better failure mode (visible overrun beats
     * silent stall). */
    if (m->indent_cells <= 0)
        return m->wrap_width;
    int min_budget = m->indent_cells + 20;
    return min_budget > m->wrap_width ? min_budget : m->wrap_width;
}

static void wrap_emit_indent(struct md_renderer *m, int n)
{
    if (n <= 0)
        return;
    /* Indent is content (is_raw=0) and goes directly to emit_cb — it
     * lives only on the terminal, not in row_buf, so a subsequent
     * wrap doesn't carry it as content to be re-emitted. */
    static const char SPACES[] = "                                ";
    while (n > 0) {
        int k = n > 32 ? 32 : n;
        m->emit_cb(SPACES, (size_t)k, 0, m->user);
        n -= k;
    }
}

/* Wrap at the recorded break (a space). Emits content+escapes up to
 * but not including the break byte, then \n, then continuation
 * indent. The break-space byte itself is dropped so the next row
 * starts at the byte after it. col is recomputed from indent_cells
 * plus the cells of the carried-over content. */
static void wrap_break(struct md_renderer *m)
{
    if (!m->indent_locked) {
        m->indent_cells = compute_indent_cells(m);
        m->indent_locked = 1;
    }
    if (m->last_break_byte > 0)
        wrap_flush_range(m, 0, (size_t)m->last_break_byte);
    m->emit_cb("\n", 1, 0, m->user);
    wrap_emit_indent(m, m->indent_cells);
    size_t shift = (size_t)m->last_break_byte + 1;
    if (shift > m->row_buf.len)
        shift = m->row_buf.len;
    size_t new_len = m->row_buf.len - shift;
    memmove(m->row_buf.data, m->row_buf.data + shift, new_len);
    memmove(m->row_meta.data, m->row_meta.data + shift, new_len);
    m->row_buf.len = new_len;
    m->row_meta.len = new_len;
    m->col = m->indent_cells + (m->col - m->last_break_col);
    m->last_break_byte = -1;
    m->last_break_col = 0;
}

/* End-of-row hard break: model emitted a \n that's a real break (not
 * a soft join). Drain row_buf as-is, emit \n, reset state. Indent is
 * cleared because the next logical line starts fresh. */
static void wrap_hard_newline(struct md_renderer *m)
{
    wrap_flush_all(m);
    m->emit_cb("\n", 1, 0, m->user);
    m->col = 0;
    m->last_break_byte = -1;
    m->last_break_col = 0;
    m->indent_cells = 0;
    m->indent_locked = 0;
    m->row_has_content = 0;
}

/* Per-codepoint append into row_buf with break-point bookkeeping.
 * Pre-wraps when the codepoint would push col past budget AND a
 * break candidate exists. A long unbroken word with no candidate
 * just keeps accumulating and overshoots the budget — the row will
 * break at the next space (the over-long word stays intact on its
 * own row). The terminal hard-wraps the over-long row visually,
 * which is the same fallback the existing reflow code uses. */
static void wrap_consume_codepoint(struct md_renderer *m, const char *out, size_t n, int cells)
{
    if (cells == 0) {
        /* Combining mark: rides on the prior glyph; append silently
         * so a wrap break can't orphan it. */
        wrap_append(m, out, n, 0);
        return;
    }
    int budget = current_row_budget(m);
    int is_space = (n == 1 && out[0] == ' ');
    if (!is_space && m->col + cells > budget && m->last_break_byte >= 0)
        wrap_break(m);
    wrap_append(m, out, n, 0);
    m->col += cells;
    if (is_space) {
        /* Record break candidate AT this space — but only if some
         * non-space content has already landed, otherwise wrapping
         * here would emit an empty row + indent + same content and
         * make no forward progress. Inline code spans are NOT
         * exempted: a soft break inside cyan stays cyan thanks to
         * SGR persistence across \n, which keeps the visual span
         * coherent without forcing the wrap to overflow. */
        if (m->row_has_content) {
            size_t new_break = m->row_buf.len - 1;
            if (m->last_break_byte < 0) {
                /* First break of the row. Detect list-marker indent
                 * now, before any streaming commit removes the
                 * marker bytes from row_buf. */
                if (!m->indent_locked) {
                    m->indent_cells = compute_indent_cells(m);
                    m->indent_locked = 1;
                }
            } else {
                /* Stream-commit through the previous break so the
                 * user sees content as it arrives instead of waiting
                 * for the row to fill or a hard \n. The previous
                 * break is now obsolete — if a wrap fires it'll
                 * prefer the latest break recorded below. col stays
                 * the same because committed bytes still occupy
                 * cells on the current terminal row. */
                size_t shift = (size_t)m->last_break_byte + 1;
                wrap_flush_range(m, 0, shift);
                size_t new_len = m->row_buf.len - shift;
                memmove(m->row_buf.data, m->row_buf.data + shift, new_len);
                memmove(m->row_meta.data, m->row_meta.data + shift, new_len);
                m->row_buf.len = new_len;
                m->row_meta.len = new_len;
                new_break -= shift;
            }
            m->last_break_byte = (int)new_break;
            m->last_break_col = m->col;
        }
    } else {
        m->row_has_content = 1;
    }
}

/* Drain any partial codepoint that cp_stream is holding. Used at
 * boundaries where ordering matters (hard \n, raw escape append) so
 * a malformed lead byte queued before the boundary doesn't end up
 * AFTER the \n or the escape in the output. */
static void wrap_drain_cp_stream(struct md_renderer *m)
{
    const char *out;
    size_t out_n;
    int cells;
    if (utf8_stream_flush(&m->cp_stream, &out, &out_n, &cells))
        wrap_consume_codepoint(m, out, out_n, cells);
}

static void emit_text_chunk(struct md_renderer *m, const char *s, size_t n)
{
    /* Wrap disabled, or modes that pass content through verbatim:
     * code fences are CommonMark-verbatim and headings are by-policy
     * single-line. */
    if (m->wrap_width <= 0 || m->in_code_fence || m->in_heading) {
        m->emit_cb(s, n, 0, m->user);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        if (b == '\n') {
            /* The step machine only emits \n via emit_text on a real
             * (hard) line break — soft joins are converted to spaces
             * before this point. Drain any pending malformed UTF-8
             * lead byte FIRST so it lands before the \n, not after. */
            wrap_drain_cp_stream(m);
            wrap_hard_newline(m);
            continue;
        }
        const char *out;
        size_t out_n;
        int cells;
        if (utf8_stream_byte(&m->cp_stream, b, &out, &out_n, &cells))
            wrap_consume_codepoint(m, out, out_n, cells);
    }
}

/* Substitute \t before content reaches the terminal so it can't expand
 * to inconsistent column-multiple-of-8 tab stops past our cell budget
 * (wcwidth('\t') == -1, so without substitution cell math counts a tab
 * as one cell while the terminal renders many). Two policies because
 * the wrap engine and the verbatim path tolerate multi-cell whitespace
 * differently:
 *
 *   - Verbatim (code fences, headings, no-wrap mode): 4 spaces.
 *     Preserves indent fidelity for source code in fenced blocks, the
 *     main practical case.
 *
 *   - Wrap engine: 1 space. The engine records a wrap-break candidate
 *     at every space and stream-commits earlier ones as later spaces
 *     arrive, so a 4-space expansion can let already-emitted spaces
 *     push the row past wrap_width by up to 3 cells before the next
 *     non-space triggers a break. A single space round-trips through
 *     the break math cleanly and is semantically fine for prose (a
 *     mid-prose tab serves the same role as a space). */
static void emit_text(struct md_renderer *m, const char *s, size_t n)
{
    int bypass = (m->wrap_width <= 0 || m->in_code_fence || m->in_heading);
    const char *tab_sub = bypass ? "    " : " ";
    size_t tab_sub_len = bypass ? 4 : 1;
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\t') {
            if (i > start)
                emit_text_chunk(m, s + start, i - start);
            emit_text_chunk(m, tab_sub, tab_sub_len);
            start = i + 1;
        }
    }
    if (start < n)
        emit_text_chunk(m, s + start, n - start);
}

/* Emit an ANSI escape (zero-width). The is_raw=1 flag tells consumers
 * routed through trail/held bookkeeping (like disp_write) to skip it,
 * so a closer after a buffered `\n` doesn't trash the trail counter. */
static void emit_raw(struct md_renderer *m, const char *s)
{
    size_t n = strlen(s);
    if (m->wrap_width <= 0 || m->in_code_fence || m->in_heading) {
        /* Drain any pending malformed UTF-8 from the wrap stream
         * even when bypassing — otherwise a transition into a
         * fence/heading would leave that byte to combine with the
         * next text in wrap mode and emit out of order. */
        if (m->wrap_width > 0)
            wrap_drain_cp_stream(m);
        m->emit_cb(s, n, 1, m->user);
        return;
    }
    /* Wrap mode: stash the escape in row_buf so it lands in the same
     * order relative to content as the model emitted it. Drain the
     * cp_stream first so any pending bytes precede the escape.
     * Re-emerged with is_raw=1 by wrap_flush_range when the row is
     * flushed. */
    wrap_drain_cp_stream(m);
    wrap_append(m, s, n, 1);
}

static void open_bold(struct md_renderer *m)
{
    emit_raw(m, ANSI_BOLD);
    m->in_bold = 1;
}

static void close_bold(struct md_renderer *m)
{
    emit_raw(m, ANSI_BOLD_OFF);
    m->in_bold = 0;
    if (m->in_heading)
        emit_raw(m, ANSI_BOLD);
}

static void open_italic(struct md_renderer *m)
{
    emit_raw(m, ANSI_ITALIC);
    m->in_italic = 1;
}

static void close_italic(struct md_renderer *m)
{
    emit_raw(m, ANSI_ITALIC_OFF);
    m->in_italic = 0;
}

static void open_inline_code(struct md_renderer *m)
{
    emit_raw(m, ANSI_CYAN);
    m->in_inline_code = 1;
}

static void close_inline_code(struct md_renderer *m)
{
    emit_raw(m, ANSI_FG_DEFAULT);
    m->in_inline_code = 0;
}

static void open_code_fence(struct md_renderer *m)
{
    /* Flip the flag before emitting so emit_raw takes the bypass
     * path (escape goes direct to emit_cb instead of buffering into
     * row_buf with content that gets re-emitted on a wrap break). */
    m->in_code_fence = 1;
    emit_raw(m, ANSI_DIM);
}

static void close_code_fence(struct md_renderer *m)
{
    emit_raw(m, ANSI_BOLD_OFF);
    m->in_code_fence = 0;
}

static void open_heading(struct md_renderer *m)
{
    /* Flag-first, see open_code_fence for rationale. */
    m->in_heading = 1;
    emit_raw(m, ANSI_BOLD);
}

static void close_heading(struct md_renderer *m)
{
    emit_raw(m, ANSI_BOLD_OFF);
    m->in_heading = 0;
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

enum step_result {
    STEP_ADVANCED,
    STEP_DEFER,
    STEP_PASS,
};

/* Inside a code fence: pass through verbatim. The only thing we look for
 * is a closing fence at line start — backticks >= opener count, followed
 * by only optional whitespace and \n (no info string per CommonMark).
 * The marker line is consumed entirely so the dim region surrounds only
 * the content lines. */
static enum step_result step_in_code_fence(struct md_renderer *m, struct buf *w, size_t *i)
{
    char c = w->data[*i];

    if (m->at_line_start && c == '`') {
        size_t cnt = 0;
        while (*i + cnt < w->len && w->data[*i + cnt] == '`')
            cnt++;
        if (*i + cnt >= w->len)
            return STEP_DEFER;
        if (cnt >= m->fence_open_count) {
            /* Skip optional trailing whitespace; \r is included
             * so a CRLF-terminated closer (`` ```\r\n ``) is
             * recognized — without it the scan would stop at \r
             * and treat the line as content. */
            size_t scan = *i + cnt;
            while (scan < w->len && w->data[scan] != '\n' &&
                   (w->data[scan] == ' ' || w->data[scan] == '\t' || w->data[scan] == '\r'))
                scan++;
            if (scan >= w->len)
                return STEP_DEFER;
            if (w->data[scan] == '\n') {
                close_code_fence(m);
                m->fence_open_count = 0;
                *i = scan + 1;
                m->at_line_start = 1;
                return STEP_ADVANCED;
            }
            /* Non-whitespace before \n — info-string-like content
             * (e.g. ```python inside a markdown demo). Fall through. */
        }
        /* Too few backticks or invalid trailing — fall through. */
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
    size_t remaining = w->len - *i;

    /* Blank line: paragraph break. Emit the \n and stay at_line_start
     * so a run of blank lines all hard-break (vs. the soft-join logic
     * in step_inline that would otherwise see the second \n in a "\n\n"
     * run as mid-paragraph). */
    if (c == '\n') {
        emit_text(m, "\n", 1);
        m->at_line_start = 1;
        m->cur_line_is_block = 0;
        (*i)++;
        return STEP_ADVANCED;
    }

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
            /* First whitespace-separated token of the info string
             * is the language label. \r is treated as whitespace
             * so a CRLF-terminated opener doesn't leak \r into
             * the rendered label. */
            size_t info = *i + cnt;
            while (info < scan &&
                   (w->data[info] == ' ' || w->data[info] == '\t' || w->data[info] == '\r'))
                info++;
            size_t lang_end = info;
            while (lang_end < scan && w->data[lang_end] != ' ' && w->data[lang_end] != '\t' &&
                   w->data[lang_end] != '\r')
                lang_end++;
            if (lang_end > info) {
                /* Dim is already on from open_code_fence; layer cyan
                 * on top so the label reads as dim cyan, distinct
                 * from the dim-default code body that follows. */
                emit_raw(m, ANSI_CYAN);
                emit_text(m, w->data + info, lang_end - info);
                emit_raw(m, ANSI_FG_DEFAULT);
                emit_text(m, "\n", 1);
            }
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
            emit_text(m, w->data + *i, k - *i + 1); /* include the \n */
            *i = k + 1;
            m->at_line_start = 1;
            return STEP_ADVANCED;
        }
        /* Not a thematic break — fall through. */
    }

    /* List marker `* ` (asterisk + space). `- ` and `1. ` don't conflict
     * with any inline marker, so they pass through as plain text without
     * special handling. */
    if (c == '*') {
        if (remaining < 2)
            return STEP_DEFER;
        if (w->data[*i + 1] == ' ') {
            emit_text(m, "*", 1);
            m->at_line_start = 0;
            (*i)++;
            return STEP_ADVANCED;
        }
        /* `**...` or `*x` — fall through. */
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
    m->wrap_width = wrap_width;
    m->last_break_byte = -1;
    return m;
}

void md_reset(struct md_renderer *m, int wrap_width)
{
    buf_reset(&m->tail);
    m->prev_byte = 0;
    m->at_line_start = 1;
    m->fence_open_count = 0;
    m->in_heading = 0;
    m->in_code_fence = 0;
    m->in_inline_code = 0;
    m->in_bold = 0;
    m->in_italic = 0;
    m->cur_line_is_block = 0;
    /* Re-sample wrap width on turn boundary so SIGWINCH-style resizes
     * between turns are picked up cheaply without callback plumbing. */
    m->wrap_width = wrap_width;
    utf8_stream_reset(&m->cp_stream);
    buf_reset(&m->row_buf);
    buf_reset(&m->row_meta);
    m->col = 0;
    m->last_break_byte = -1;
    m->last_break_col = 0;
    m->indent_cells = 0;
    m->indent_locked = 0;
    m->row_has_content = 0;
}

void md_free(struct md_renderer *m)
{
    if (!m)
        return;
    buf_free(&m->tail);
    buf_free(&m->row_buf);
    buf_free(&m->row_meta);
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

        if (m->at_line_start) {
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
     * excess is flushed as plain text rather than held forever. */
    size_t rem = w.len - i;
    if (rem > TAIL_MAX) {
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

    if (m->in_code_fence && m->tail.len > 0) {
        size_t cnt = 0;
        while (cnt < m->tail.len && m->tail.data[cnt] == '`')
            cnt++;
        if (cnt >= m->fence_open_count) {
            int valid = 1;
            for (size_t i = cnt; i < m->tail.len; i++) {
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
                /* Hard break: emit \n then the marker line content
                 * (leading whitespace before the marker is dropped
                 * per CommonMark equivalence). */
                emit_text(m, "\n", 1);
                size_t rest_len = m->tail.len - k;
                char *rest = xmalloc(rest_len);
                memcpy(rest, m->tail.data + k, rest_len);
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

    /* Drain any partial codepoint and any residual content+escapes
     * that the wrap layer has been holding (the row was still in
     * progress at end-of-stream — no trailing \n to trigger a hard
     * break). Order: utf8 flush so a truncated multi-byte sequence
     * lands in row_buf as malformed bytes; then wrap_flush_all to
     * push the row to the terminal. No \n appended — this is a
     * partial row, the caller's block separator handles spacing. */
    if (m->wrap_width > 0) {
        const char *out;
        size_t out_n;
        int cells;
        if (utf8_stream_flush(&m->cp_stream, &out, &out_n, &cells))
            wrap_consume_codepoint(m, out, out_n, cells);
        wrap_flush_all(m);
    }
}
