/* SPDX-License-Identifier: MIT */
#include "render/markdown.h"

#include <stdio.h>
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

/* Content cells granted past the indent only on a doomed continuation row
 * (indent_cells >= wrap_width) — see current_row_budget. Small: just enough
 * to print a short word per row instead of crawling one column at a time. */
#define WRAP_MIN_CONTENT 8

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
     * e.g. inline code (cyan) inside bold leaves bold intact when the code
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

    /* ---------- wrap layer ----------
     *
     * Active when wrap_width > 0. Sits between the markdown step machine
     * and the emit callback: every codepoint (and every SGR escape) is
     * passed through to emit_cb eagerly, so the terminal sees each byte
     * the moment it arrives. row_buf + row_meta keep a shadow of the
     * current logical row — content + interleaved escapes, tagged via
     * row_meta (0 = content, 1 = raw) — so wrap_break can replay the
     * partial word after issuing a cursor-back-and-erase, and so
     * compute_indent_cells can inspect the row's leading bytes at the
     * first break. Codepoints are reassembled by cp_stream so cell
     * counts are exact even when step_inline hands us bytes mid-
     * sequence.
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
     * wrap_break erases row_buf[break_byte..] (the space + the partial
     * word) from the terminal and replays row_buf[break_byte+1..] on
     * the next row, dropping the space itself. */
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

    /* Set when a break-space would have landed at col > wrap_width.
     * Instead of emitting it (which would commit xterm's delayed-
     * autowrap before our \n could fire), we drop the space and
     * defer the actual \n + indent until the next byte arrives —
     * either a non-\n that commits the wrap, or a hard \n that
     * subsumes it. Pending_wrap means: "owe a \n + indent before
     * the next byte on the wire". Avoids the held-space gymnastics
     * (which had its own autowrap and SGR-ordering corner cases). */
    int pending_wrap;

    /* Snapshot of in_bold / in_italic / in_inline_code at the moment
     * the most-recent break-space was recorded. wrap_break uses this
     * to restore terminal SGR state before replaying the partial word
     * on the new row: SGRs are eagerly emitted, so a style transition
     * inside the erased slice (e.g. `**foo bar**baz` closing bold
     * before the wrap point) has already mutated terminal state by the
     * time CSI nD fires. Without the restore, the replayed bytes
     * inherit the wrong state — `bar` on row 2 would render unbold
     * even though it precedes the bold closer in source order. */
    int snap_in_bold;
    int snap_in_italic;
    int snap_in_inline_code;
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

static void wrap_emit_indent(struct md_renderer *m, int n);

/* Commit a deferred edge-wrap to the wire: emit \n + indent and
 * reset row state so the next byte lands at column 0 of a fresh
 * row. Called from the top of wrap_consume_codepoint and wrap_append
 * — every entry point that would put a new byte on the terminal
 * — so the pending \n always lands before the byte that triggered
 * the commit. wrap_hard_newline absorbs pending instead (its own
 * \n is what the edge wrap was waiting for). */
static void commit_pending_wrap(struct md_renderer *m)
{
    if (!m->pending_wrap)
        return;
    m->pending_wrap = 0;
    m->emit_cb("\n", 1, 0, m->user);
    wrap_emit_indent(m, m->indent_cells);
    buf_reset(&m->row_buf);
    buf_reset(&m->row_meta);
    m->col = m->indent_cells;
    m->last_break_byte = -1;
    m->last_break_col = 0;
    m->row_has_content = 0;
    m->snap_in_bold = 0;
    m->snap_in_italic = 0;
    m->snap_in_inline_code = 0;
}

/* Append bytes to row_buf with their is_raw kind tagged in row_meta,
 * and emit them straight through to the terminal so the user sees
 * each codepoint the moment it arrives — no waiting on a downstream
 * commit point. The row_buf shadow is kept anyway: indent detection
 * still inspects the row's prefix at the first break, and wrap_break
 * uses it to replay the partial word on the next row after erasing
 * the eagerly-emitted bytes that overshot the budget. The two arrays
 * stay in lockstep so wrap_flush_range can re-emit runs of like-kind
 * bytes through emit_cb with the correct flag during a retro-wrap.
 *
 * The autowrap-edge case (a break-space that would land at col >
 * wrap_width) is handled upstream in wrap_consume_codepoint, which
 * drops the space and sets pending_wrap instead of calling here. So
 * by the time we get to wrap_append, the byte is safe to put on the
 * wire — we just commit any pending edge-wrap first so the \n + new
 * row lands before the new byte.
 *
 * Exception: zero-width raw bytes (SGR escapes) during a pending
 * edge-wrap go straight to the wire without committing. Committing
 * on a zero-width byte would emit \n + indent ahead of any visible
 * content, stranding the \n on its own line and producing an
 * extra blank row when the next byte is a hard \n that would
 * otherwise absorb pending. Only a visible byte (commit) or a
 * hard \n (absorb) should resolve pending_wrap. The SGR is not
 * stored in row_buf during this window — row_buf still holds the
 * prior (full) row, which commit_pending_wrap will reset anyway,
 * and no wrap_break can fire before commit since pending_wrap
 * means we're already past the edge with no more bytes appended
 * to the current row's shadow. */
static void wrap_append(struct md_renderer *m, const char *s, size_t n, int is_raw)
{
    if (is_raw && m->pending_wrap) {
        m->emit_cb(s, n, 1, m->user);
        return;
    }
    commit_pending_wrap(m);
    buf_append(&m->row_buf, s, n);
    char meta = is_raw ? 1 : 0;
    for (size_t i = 0; i < n; i++)
        buf_append(&m->row_meta, &meta, 1);
    m->emit_cb(s, n, is_raw, m->user);
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

/* End-of-row / end-of-stream cleanup. Every byte that ever reached
 * row_buf has already been emitted to the terminal (wrap_append is
 * the only entry, and it emits unconditionally), so this is just a
 * state reset of the shadow buffers. Pending_wrap stays set across
 * this — it's per-renderer state that the next byte (next feed, or
 * a hard \n) commits or absorbs; md_reset clears it at turn
 * boundaries. */
static void wrap_flush_all(struct md_renderer *m)
{
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
 *   no marker          → leading_spaces (align wrapped rows under the
 *                        line's own indent — a list-item continuation
 *                        paragraph or code/blockquote body keeps its
 *                        hanging indent instead of wrapping back to
 *                        column 0)
 * ASCII-only patterns, so byte offsets equal cell counts. The
 * leading-space scan is capped at 8 to keep the continuation indent
 * within sane budget territory; deeper nesting levels just lose a bit
 * of visual hierarchy on wrap. */
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
    /* No marker — align continuation rows under the line's own leading
     * indent (0 for a plain top-level paragraph). */
    return (int)lead_spaces;
}

/* Continuation budget for a wrapped row. Almost always wrap_width —
 * which md_wrap_width() keeps one cell inside the physical edge, so
 * the eager retro-wrap (CSI nD / CSI K) never touches the last column
 * and survives terminals that defer the autowrap (libvterm's phantom
 * column). A list-item continuation row reserves indent_cells of that
 * width for the hanging indent, but the budget stays at wrap_width so
 * the reserved column is honored regardless of indent depth.
 *
 * The sole exception is a pathologically deep bullet on a tiny
 * terminal where the indent alone meets or exceeds wrap_width: the row
 * is already doomed to the terminal's own hard-wrap, so capping at
 * wrap_width would only crawl one column at a time. There we overshoot
 * to keep the content legible — the phantom hazard is unreachable on a
 * row whose indent already fills the usable width. */
static int current_row_budget(struct md_renderer *m)
{
    if (m->indent_cells <= 0 || m->indent_cells < m->wrap_width)
        return m->wrap_width;
    return m->indent_cells + WRAP_MIN_CONTENT;
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

/* Retroactive wrap: the partial word past the recorded break-space
 * was eagerly emitted onto the current row and now would overshoot
 * the budget. Move the terminal cursor back over the break-space and
 * the partial word, erase to end of line, drop down to a new row with
 * the continuation indent, and replay the partial word from row_buf.
 * The break-space itself is dropped (not replayed) so the next row
 * starts at the first byte of the partial word — same final visible
 * state as the legacy buffered-flush wrap, just delivered without
 * waiting for the next codepoint to commit the prior word. */
static void wrap_break(struct md_renderer *m)
{
    if (!m->indent_locked) {
        m->indent_cells = compute_indent_cells(m);
        m->indent_locked = 1;
    }
    /* erase_cells = partial word's cells + 1 for the break-space.
     * Every byte in row_buf has been emitted (no holds in this
     * design), so the break-space always contributes 1 cell to the
     * erase. col is pre-new-codepoint, last_break_col is post-space. */
    int erase_cells = (m->col - m->last_break_col) + 1;
    if (erase_cells > 0) {
        char esc[16];
        int len = snprintf(esc, sizeof(esc), "\x1b[%dD", erase_cells);
        m->emit_cb(esc, (size_t)len, 1, m->user);
        m->emit_cb(ANSI_ERASE_LINE, sizeof(ANSI_ERASE_LINE) - 1, 1, m->user);
    }
    m->emit_cb("\n", 1, 0, m->user);
    wrap_emit_indent(m, m->indent_cells);
    /* Restore terminal SGR state to the snapshot taken at the break:
     * post-break style transitions have already eagerly mutated
     * terminal state, so the replay would otherwise inherit the
     * wrong state for any byte that's inside the snapshot's span
     * but past a closer that fired before wrap_break. Skipped in
     * unstyled mode where the renderer emits no SGRs at all (any
     * close here would clobber the caller's outer span). */
    if (m->styled) {
        if (m->in_bold != m->snap_in_bold) {
            const char *e = m->snap_in_bold ? ANSI_BOLD : ANSI_BOLD_OFF;
            m->emit_cb(e, strlen(e), 1, m->user);
        }
        if (m->in_italic != m->snap_in_italic) {
            const char *e = m->snap_in_italic ? ANSI_ITALIC : ANSI_ITALIC_OFF;
            m->emit_cb(e, strlen(e), 1, m->user);
        }
        if (m->in_inline_code != m->snap_in_inline_code) {
            const char *e = m->snap_in_inline_code ? ANSI_CYAN : ANSI_FG_DEFAULT;
            m->emit_cb(e, strlen(e), 1, m->user);
        }
    }
    /* Replay the partial word + any escapes interleaved with it. The
     * break-space at offset last_break_byte is skipped. */
    size_t shift = (size_t)m->last_break_byte + 1;
    if (shift > m->row_buf.len)
        shift = m->row_buf.len;
    if (shift < m->row_buf.len)
        wrap_flush_range(m, shift, m->row_buf.len);
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
 * a soft join). Reset row state, emit \n. A pending edge-wrap is
 * absorbed here — its deferred \n is exactly what we're about to
 * emit, so committing it separately would double-up the line break. */
static void wrap_hard_newline(struct md_renderer *m)
{
    m->pending_wrap = 0;
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
 *
 * Two wrap paths:
 *   - Edge-space wrap: a break-space that would land at col >
 *     wrap_width gets dropped on the floor; pending_wrap is set so
 *     the next byte (commit_pending_wrap) or hard \n (subsume)
 *     supplies the actual line break. Realistically there's no
 *     other outcome — the space IS the wrap point, anything after
 *     belongs on the next row.
 *   - Mid-word wrap: a non-space that would overshoot fires
 *     wrap_break at the last break-space — erase the partial word,
 *     drop down, replay (with SGR rewind from snap_in_X).
 *
 * Pending_wrap is committed at entry so a second consecutive byte
 * (be it another space, content, or an SGR via wrap_append) lands
 * on the fresh row with row_has_content cleared. A long unbroken
 * word with no break candidate just keeps streaming past budget —
 * the terminal hard-wraps it, same fallback as before. */
static void wrap_consume_codepoint(struct md_renderer *m, const char *out, size_t n, int cells)
{
    /* CR is the lead of a CRLF line ending, not a glyph. Drop it in the
     * wrap layer: the \n that follows drives the break, so a bare CR
     * here would only commit a pending wrap (turning a CRLF hard break
     * landing on the wrap boundary into an extra blank line) or strand
     * the cursor at column 0 mid-row. The trailing-space counter in
     * emit_text already treats \r as transparent, so the hard-break
     * rule still sees the spaces before it. */
    if (n == 1 && out[0] == '\r')
        return;
    /* A space arriving while a wrap is already pending belongs to a run
     * of spaces straddling the wrap point. Drop it instead of letting it
     * commit the deferred \n: committing would strand the space at
     * column 0 of the next row, which turned trailing hard-break spaces
     * landing on the wrap boundary into a spurious blank/space line
     * (`aaaaa  \nbar` at width 5 → `aaaaa\n \nbar`) and left ragged
     * leading spaces on ordinary wrapped rows. Only non-space content
     * (or a hard \n, which subsumes pending) commits the wrap. */
    if (n == 1 && out[0] == ' ' && m->pending_wrap)
        return;
    commit_pending_wrap(m);
    if (cells == 0) {
        /* Combining mark: rides on the prior glyph; append silently
         * so a wrap break can't orphan it. */
        wrap_append(m, out, n, 0);
        return;
    }
    int budget = current_row_budget(m);
    int is_space = (n == 1 && out[0] == ' ');
    /* Edge-space: don't emit, defer the wrap. row_has_content is
     * the same guard we use for break-recording — leading spaces
     * never set pending. */
    if (is_space && m->row_has_content && m->col + cells > budget) {
        m->pending_wrap = 1;
        return;
    }
    /* Mid-word overflow: wrap at the last break-space. */
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
            if (m->last_break_byte < 0 && !m->indent_locked) {
                /* First break of the row. Detect list-marker indent
                 * now, before the streaming commit below drops the
                 * marker bytes from row_buf. */
                m->indent_cells = compute_indent_cells(m);
                m->indent_locked = 1;
            }
            /* Trim row_buf so it retains only the break-space and any
             * future partial word past it — eager-emit already pushed
             * everything before this space onto the terminal, so the
             * shadow only needs to hold what wrap_break would replay
             * after an erase. Keeping the break-space as the new
             * row_buf head lets wrap_break compute the erase span
             * (cursor back through the space + partial word) and
             * skip the space on replay so no trailing-space artifact
             * survives the wrap. */
            size_t shift = new_break;
            if (shift > 0) {
                size_t new_len = m->row_buf.len - shift;
                memmove(m->row_buf.data, m->row_buf.data + shift, new_len);
                memmove(m->row_meta.data, m->row_meta.data + shift, new_len);
                m->row_buf.len = new_len;
                m->row_meta.len = new_len;
                new_break -= shift;
            }
            m->last_break_byte = (int)new_break;
            m->last_break_col = m->col;
            /* Capture the active SGR state at this break point so
             * wrap_break can rewind terminal state before replaying
             * — eager-emitted SGRs after the space might leave the
             * terminal in a different state than the replay expects. */
            m->snap_in_bold = m->in_bold;
            m->snap_in_italic = m->in_italic;
            m->snap_in_inline_code = m->in_inline_code;
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
 *     at every space and trims earlier ones from row_buf as later
 *     spaces arrive, so a 4-space expansion would let the eagerly-
 *     emitted spaces push the row past wrap_width by up to 3 cells
 *     before the next non-space triggers a break. A single space
 *     round-trips through the break math cleanly and is semantically
 *     fine for prose (a mid-prose tab serves the same role as a
 *     space). */
static void emit_text(struct md_renderer *m, const char *s, size_t n)
{
    /* Track the trailing-space run for the hard-line-break rule (see the
     * \n handler in step_inline). Counted on the raw bytes before tab
     * substitution: a tab is not a space for CommonMark's rule, and any
     * non-space resets the run — except \r, which is transparent so a
     * CRLF line ending ("foo  \r\n") still sees the two spaces. */
    for (size_t j = 0; j < n; j++) {
        if (s[j] == ' ')
            m->trailing_spaces++;
        else if (s[j] != '\r')
            m->trailing_spaces = 0;
    }

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
    /* Unstyled mode: every emit_raw call site is an SGR escape, so a
     * single gate here suppresses them all. The in_* flag bookkeeping
     * in the open_/close_ helpers still runs, keeping marker nesting
     * correct. */
    if (!m->styled)
        return;
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
    /* Wrap mode: drain any pending UTF-8 lead byte first so it lands
     * before the escape, then wrap_append emits the escape eagerly
     * (is_raw=1) and also stores it in row_buf so a subsequent
     * wrap_break replay re-emits it inline with the partial word's
     * content bytes. */
    wrap_drain_cp_stream(m);
    wrap_append(m, s, n, 1);
}

/* An inline delimiter (`*`, `**`, `_`, `` ` ``) is a non-space source
 * byte that's consumed without passing through emit_text, so it must
 * clear the trailing-space run itself — otherwise spaces sitting before
 * the delimiter (e.g. `foo  **`) would leave a stale count and a later
 * \n would wrongly read as a hard break. */
static void open_bold(struct md_renderer *m)
{
    emit_raw(m, ANSI_BOLD);
    m->in_bold = 1;
    m->trailing_spaces = 0;
}

static void close_bold(struct md_renderer *m)
{
    emit_raw(m, ANSI_BOLD_OFF);
    m->in_bold = 0;
    m->trailing_spaces = 0;
    if (m->in_heading)
        emit_raw(m, ANSI_BOLD);
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
    emit_raw(m, ANSI_CYAN);
    m->in_inline_code = 1;
    m->trailing_spaces = 0;
}

static void close_inline_code(struct md_renderer *m)
{
    emit_raw(m, ANSI_FG_DEFAULT);
    m->in_inline_code = 0;
    m->trailing_spaces = 0;
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
    m->styled = 1;
    m->wrap_width = wrap_width;
    m->last_break_byte = -1;
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
    m->pending_wrap = 0;
    m->snap_in_bold = 0;
    m->snap_in_italic = 0;
    m->snap_in_inline_code = 0;
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

    /* Drain any partial UTF-8 codepoint the wrap layer was holding
     * (truncated multi-byte sequence at end-of-stream), then clear
     * the shadow row_buf. With eager emit the row's bytes are already
     * on the terminal, so wrap_flush_all is just a state reset; we
     * don't append a \n — this is a partial row and the caller's
     * block separator handles spacing. */
    if (m->wrap_width > 0) {
        const char *out;
        size_t out_n;
        int cells;
        if (utf8_stream_flush(&m->cp_stream, &out, &out_n, &cells))
            wrap_consume_codepoint(m, out, out_n, cells);
        wrap_flush_all(m);
    }
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
    md_reset(m, m->wrap_width);
    m->styled = on;
}

int md_cursor_col(const struct md_renderer *m)
{
    if (!m || m->wrap_width <= 0)
        return 0;
    /* Every appended byte was emitted (no held-state), so col is
     * the terminal cursor column. A pending edge-wrap leaves the
     * cursor at col = wrap_width (the dropped space's would-be
     * landing column never reached the wire). */
    return m->col;
}
