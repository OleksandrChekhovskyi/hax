/* SPDX-License-Identifier: MIT */
#include "terminal/input_core.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "terminal/input.h"
#include "text/utf8.h"

/* ---------------- public API: alloc / free ---------------- */

struct input *input_new(void)
{
    struct input *in = xcalloc(1, sizeof(*in));
    in->buf = xmalloc(64);
    in->cap = 64;
    in->buf[0] = '\0';
    return in;
}

void input_free(struct input *in)
{
    if (!in)
        return;
    free(in->buf);
    free(in->draft);
    for (size_t i = 0; i < in->hist_n; i++)
        free(in->hist[i]);
    free(in->hist);
    free(in->persist_path);
    free(in);
}

int input_core_prompt_width(const char *s)
{
    int w = 0;
    size_t i = 0, len = strlen(s);
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n')
            break;
        if (c == 0x1b && i + 1 < len && s[i + 1] == '[') {
            i += 2;
            while (i < len && (unsigned char)s[i] < 0x40)
                i++;
            if (i < len)
                i++;
            continue;
        }
        size_t consumed;
        int cw = utf8_codepoint_cells(s, len, i, &consumed);
        /* Substituted glyphs (controls, dangerous) render as 1 col;
         * mirror that here so a stray non-printable in a prompt can't
         * make the width go negative and corrupt continuation indent. */
        if (cw < 0)
            cw = 1;
        w += cw;
        i += consumed ? consumed : 1;
    }
    return w;
}

/* ---------------- buffer ops ---------------- */

static void buf_grow(struct input *in, size_t need)
{
    if (need <= in->cap)
        return;
    size_t cap = in->cap ? in->cap : 64;
    while (cap < need)
        cap *= 2;
    in->buf = xrealloc(in->buf, cap);
    in->cap = cap;
}

void input_core_buf_set(struct input *in, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    buf_grow(in, n + 1);
    if (n)
        memcpy(in->buf, s, n);
    in->buf[n] = '\0';
    in->len = n;
    in->cursor = n;
}

void input_core_buf_insert(struct input *in, const char *bytes, size_t n)
{
    if (n == 0)
        return;
    buf_grow(in, in->len + n + 1);
    memmove(in->buf + in->cursor + n, in->buf + in->cursor, in->len - in->cursor);
    memcpy(in->buf + in->cursor, bytes, n);
    in->len += n;
    in->cursor += n;
    in->buf[in->len] = '\0';
}

static void buf_erase(struct input *in, size_t pos, size_t n)
{
    if (pos >= in->len || n == 0)
        return;
    if (pos + n > in->len)
        n = in->len - pos;
    memmove(in->buf + pos, in->buf + pos + n, in->len - pos - n);
    in->len -= n;
    in->buf[in->len] = '\0';
    if (in->cursor > pos + n)
        in->cursor -= n;
    else if (in->cursor > pos)
        in->cursor = pos;
}

/* ---------------- motions / edits ---------------- */

size_t input_core_line_start(const struct input *in)
{
    size_t i = in->cursor;
    while (i > 0 && in->buf[i - 1] != '\n')
        i--;
    return i;
}

size_t input_core_line_end(const struct input *in)
{
    size_t i = in->cursor;
    while (i < in->len && in->buf[i] != '\n')
        i++;
    return i;
}

void input_core_move_left(struct input *in)
{
    if (in->cursor > 0)
        in->cursor = utf8_prev(in->buf, in->cursor);
}

void input_core_move_right(struct input *in)
{
    if (in->cursor < in->len)
        in->cursor = utf8_next(in->buf, in->len, in->cursor);
}

void input_core_delete_back(struct input *in)
{
    if (in->cursor == 0)
        return;
    size_t prev = utf8_prev(in->buf, in->cursor);
    buf_erase(in, prev, in->cursor - prev);
}

void input_core_delete_fwd(struct input *in)
{
    if (in->cursor >= in->len)
        return;
    size_t next = utf8_next(in->buf, in->len, in->cursor);
    buf_erase(in, in->cursor, next - in->cursor);
}

void input_core_kill_to_eol(struct input *in)
{
    size_t e = input_core_line_end(in);
    if (e == in->cursor && e < in->len && in->buf[e] == '\n')
        e++; /* on empty line, eat the newline so Ctrl-K joins lines */
    buf_erase(in, in->cursor, e - in->cursor);
}

void input_core_kill_to_bol(struct input *in)
{
    size_t b = input_core_line_start(in);
    buf_erase(in, b, in->cursor - b);
}

/* Two word-boundary flavors, matching readline:
 *   - whitespace scan: only ASCII whitespace breaks words. Used by
 *     Ctrl-W (`unix-word-rubout`) so "rm foo/bar" → one Ctrl-W deletes
 *     the whole path, the long-standing shell idiom.
 *   - alnum scan: any non-alnum ASCII byte breaks (so `/`, `.`, `-`,
 *     `_`... no wait, `_` is alnum-adjacent — readline treats only
 *     alnum as word chars, so `_` is a boundary too). Used by Meta-
 *     bound ops (M-b/M-f/M-d/M-DEL) and the modified-arrow CSI keys,
 *     matching `backward-word` / `forward-word` / `kill-word` /
 *     `backward-kill-word`.
 *
 * Both scans are byte-wise. UTF-8 continuation/leader bytes are all
 * >= 0x80; the alnum predicate explicitly treats those as word chars,
 * so multi-byte letters (é, ö, …) stay inside words instead of
 * splitting mid-codepoint. The whitespace predicate already handles
 * UTF-8 correctly because isspace returns 0 for any byte >= 0x80. */
static size_t scan_ws_left(const char *buf, size_t i)
{
    while (i > 0 && isspace((unsigned char)buf[i - 1]))
        i--;
    while (i > 0 && !isspace((unsigned char)buf[i - 1]))
        i--;
    return i;
}

static int is_word_byte(unsigned char c)
{
    return c >= 0x80 || isalnum(c);
}

static size_t scan_alnum_left(const char *buf, size_t i)
{
    while (i > 0 && !is_word_byte((unsigned char)buf[i - 1]))
        i--;
    while (i > 0 && is_word_byte((unsigned char)buf[i - 1]))
        i--;
    return i;
}

static size_t scan_alnum_right(const char *buf, size_t len, size_t i)
{
    while (i < len && !is_word_byte((unsigned char)buf[i]))
        i++;
    while (i < len && is_word_byte((unsigned char)buf[i]))
        i++;
    return i;
}

void input_core_move_word_left(struct input *in)
{
    in->cursor = scan_alnum_left(in->buf, in->cursor);
}

void input_core_move_word_right(struct input *in)
{
    in->cursor = scan_alnum_right(in->buf, in->len, in->cursor);
}

void input_core_kill_word_back(struct input *in)
{
    size_t i = scan_ws_left(in->buf, in->cursor);
    buf_erase(in, i, in->cursor - i);
}

void input_core_kill_word_back_alnum(struct input *in)
{
    size_t i = scan_alnum_left(in->buf, in->cursor);
    buf_erase(in, i, in->cursor - i);
}

void input_core_kill_word_fwd(struct input *in)
{
    size_t e = scan_alnum_right(in->buf, in->len, in->cursor);
    buf_erase(in, in->cursor, e - in->cursor);
}

/* ---------------- history ---------------- */

/* Append `line` to history without touching any persistence layer.
 * Erases any prior exact-match occurrences first (zsh
 * HIST_IGNORE_ALL_DUPS / bash HISTCONTROL=erasedups semantics) so a
 * recalled entry bumps to the top instead of duplicating — the same
 * canned prompts get reused constantly in a coding-agent REPL.
 *
 * Fast path: if `line` is already the most-recent entry, the erasedups
 * would self-cancel (erase idx hist_n-1, then re-append the same
 * string). Skip outright — saves the on-disk wrapper an append too. */
int input_core_history_add(struct input *in, const char *line)
{
    if (!line || !*line)
        return 0;
    if (in->hist_n > 0 && strcmp(in->hist[in->hist_n - 1], line) == 0)
        return 0;
    /* Erase prior exact matches. Walk back-to-front so indices stay
     * valid as we remove. */
    for (size_t i = in->hist_n; i > 0; i--) {
        if (strcmp(in->hist[i - 1], line) == 0) {
            free(in->hist[i - 1]);
            memmove(&in->hist[i - 1], &in->hist[i], (in->hist_n - i) * sizeof(char *));
            in->hist_n--;
        }
    }
    if (in->hist_n + 1 > in->hist_cap) {
        in->hist_cap = in->hist_cap ? in->hist_cap * 2 : 16;
        in->hist = xrealloc(in->hist, in->hist_cap * sizeof(char *));
    }
    in->hist[in->hist_n++] = xstrdup(line);
    if (in->hist_n > INPUT_CORE_HISTORY_MAX) {
        free(in->hist[0]);
        memmove(&in->hist[0], &in->hist[1], (in->hist_n - 1) * sizeof(char *));
        in->hist_n--;
    }
    return 1;
}

/* ---------------- history persistence (encode/decode) ---------------- */

/* Encode an entry for the on-disk one-line-per-record format: literal
 * backslash -> "\\", literal LF -> "\n". Caller frees. The result has
 * no trailing newline — the file writer adds one. */
char *input_core_history_encode(const char *s)
{
    if (!s)
        return xstrdup("");
    size_t n = strlen(s);
    char *out = xmalloc(n * 2 + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') {
            out[j++] = '\\';
            out[j++] = '\\';
        } else if (c == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    return out;
}

/* Decode `n` bytes of an encoded entry. Recognizes "\\" -> '\\' and
 * "\n" -> LF. An unrecognized escape (or trailing backslash) is
 * preserved verbatim — forward-compatible with future escape additions
 * and resilient to a hand-edited file. Caller frees. */
char *input_core_history_decode(const char *s, size_t n)
{
    char *out = xmalloc(n + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            char nx = s[i + 1];
            if (nx == '\\') {
                out[j++] = '\\';
                i++;
                continue;
            }
            if (nx == 'n') {
                out[j++] = '\n';
                i++;
                continue;
            }
        }
        out[j++] = s[i];
    }
    out[j] = '\0';
    return out;
}

/* History navigation convention:
 *
 * `hist_pos` ranges over [0, hist_n]. Values < hist_n point at a
 * recalled entry; the sentinel `hist_pos == hist_n` means "we're on
 * the user's current draft, not a history entry".
 *
 * On the first Up out of the draft, save the current buffer to
 * `draft`; subsequent Ups don't re-save (the user is browsing
 * history). On Down past the last entry, restore from `draft` and
 * free it. Edits made to a recalled entry are local to the buffer —
 * `hist[i]` strings are never mutated, so navigating away discards
 * those edits.
 */
static void hist_save_draft(struct input *in)
{
    free(in->draft);
    in->draft = xstrdup(in->buf);
}

static void hist_load(struct input *in, size_t pos)
{
    if (pos < in->hist_n) {
        input_core_buf_set(in, in->hist[pos]);
        in->hist_pos = pos;
    } else {
        input_core_buf_set(in, in->draft ? in->draft : "");
        free(in->draft);
        in->draft = NULL;
        in->hist_pos = in->hist_n;
    }
}

void input_core_history_prev(struct input *in)
{
    if (in->hist_n == 0)
        return;
    if (in->hist_pos == in->hist_n)
        hist_save_draft(in);
    if (in->hist_pos > 0)
        hist_load(in, in->hist_pos - 1);
}

void input_core_history_next(struct input *in)
{
    if (in->hist_pos >= in->hist_n)
        return;
    hist_load(in, in->hist_pos + 1);
}

/* ---------------- render walker ---------------- */

/* One decoded glyph from the source buffer. For ASCII tabs and unsafe
 * bytes, `bytes` points at a static substitute (so the caller can write
 * it verbatim); for valid codepoints, `bytes` slices `buf` in place.
 * `consumed` is the number of source bytes this glyph covers. */
struct walk_glyph {
    const char *bytes;
    size_t n;
    int width;
    size_t byte_index;
    size_t consumed;
    int is_space; /* ASCII ' ' — a candidate word-wrap boundary */
};

/* Substitute glyph for non-printables (C0/C1 controls, malformed UTF-8,
 * dangerous bidi/format codepoints). Same '?' the legacy emit_safe_span
 * used; substituting at the walker level means every rendering site —
 * editor paint, submitted-message paint, layout math — sees the same
 * cell count. */
static const char SUBST_BYTES[1] = {'?'};

/* Tab substitute: rendered as INPUT_CORE_TAB_WIDTH spaces. Stored as a
 * static buffer so callers can point at it without owning storage; the
 * walker only emits up to INPUT_CORE_TAB_WIDTH bytes from it. */
static const char TAB_SPACES[INPUT_CORE_TAB_WIDTH] = {' ', ' ', ' ', ' '};

static int decode_glyph(const char *buf, size_t len, size_t i, struct walk_glyph *g)
{
    if (i >= len)
        return 0;
    unsigned char c = (unsigned char)buf[i];
    g->byte_index = i;
    if (c == '\t') {
        g->bytes = TAB_SPACES;
        g->n = INPUT_CORE_TAB_WIDTH;
        g->width = INPUT_CORE_TAB_WIDTH;
        g->consumed = 1;
        g->is_space = 0;
        return 1;
    }
    if (c < 0x20 || c == 0x7f) {
        /* C0 control or DEL: substitute one '?'. Matches the old
         * emit_safe_span policy of refusing to write raw control bytes
         * that some terminals interpret as escape sequences. */
        g->bytes = SUBST_BYTES;
        g->n = 1;
        g->width = 1;
        g->consumed = 1;
        g->is_space = 0;
        return 1;
    }
    size_t consumed;
    int w = utf8_codepoint_cells(buf, len, i, &consumed);
    if (w < 0) {
        /* Non-printable codepoint (C1 in UTF-8, format chars, malformed
         * sequence). Substitute one '?' for the leader byte; the
         * caller advances by 1 so each problem byte gets its own
         * substitute. */
        g->bytes = SUBST_BYTES;
        g->n = 1;
        g->width = 1;
        g->consumed = consumed ? consumed : 1;
        g->is_space = 0;
        return 1;
    }
    g->bytes = buf + i;
    g->n = consumed;
    g->width = w; /* may be 0 for combining marks */
    g->consumed = consumed ? consumed : 1;
    g->is_space = (consumed == 1 && c == ' ');
    return 1;
}

/* Walker state. Glyphs since the last committed wrap candidate (row
 * start or last emitted space) live in `wbuf` so a word that runs past
 * `cols` can be replayed onto the next row instead of breaking mid-
 * token. The buffer is fixed-size; an unbroken token longer than the
 * buffer (a pathological URL / hash) is committed as it grows, with
 * char-wrap behavior taking over.
 *
 * `cursor_resolved` is set when the walker sees a byte index that
 * lies inside the cursor's byte range; the cursor's row/col is taken
 * from the glyph's *final* emit position (after any wrap replay), not
 * its pre-buffer position.
 *
 * Wrap policy: post-emit column must stay strictly less than `cols`.
 * The terminal's last cell (col == cols-1) is the "phantom" position
 * — most terminals enter delayed-wrap state when the cursor advances
 * past it, and CUF (`\x1b[NC`) clamps to col cols-1, which would put
 * a cursor positioned at col=cols visually on top of the last glyph.
 * Triggering wrap one column earlier (post-emit >= cols ⇒ wrap)
 * keeps the cursor unambiguously in [0, cols-1] for every row. */
#define WBUF_CAP 256

struct walker {
    int prompt_w;
    int cont_indent;
    int cols;
    size_t cursor;
    input_render_cb cb;
    void *user;

    /* Position at which the next committed glyph would emit. */
    int row;
    int col;

    int cursor_row;
    int cursor_col;
    int cursor_resolved;

    /* Pending tail (current word). */
    struct walk_glyph wbuf[WBUF_CAP];
    size_t wbuf_n;
    int wbuf_width;       /* sum of widths in wbuf */
    int wbuf_after_space; /* wbuf may replay to next row at a word boundary */
};

static int cursor_in_range(size_t cursor, size_t start, size_t consumed)
{
    return cursor >= start && cursor < start + consumed;
}

static void emit_glyph(struct walker *w, const struct walk_glyph *g)
{
    if (!w->cursor_resolved && cursor_in_range(w->cursor, g->byte_index, g->consumed)) {
        w->cursor_row = w->row;
        w->cursor_col = w->col;
        w->cursor_resolved = 1;
    }
    if (w->cb) {
        struct input_render_event ev = {.kind = INPUT_RENDER_GLYPH,
                                        .bytes = g->bytes,
                                        .n = g->n,
                                        .width = g->width,
                                        .row = w->row,
                                        .col = w->col};
        w->cb(&ev, w->user);
    }
    w->col += g->width;
}

static void emit_row_break(struct walker *w)
{
    w->row++;
    w->col = w->cont_indent;
    if (w->cb) {
        struct input_render_event ev = {.kind = INPUT_RENDER_ROW_BREAK,
                                        .bytes = NULL,
                                        .n = 0,
                                        .width = 0,
                                        .row = w->row,
                                        .col = w->col};
        w->cb(&ev, w->user);
    }
}

static void wbuf_flush(struct walker *w)
{
    for (size_t i = 0; i < w->wbuf_n; i++)
        emit_glyph(w, &w->wbuf[i]);
    w->wbuf_n = 0;
    w->wbuf_width = 0;
    w->wbuf_after_space = 0;
}

/* Move the contents of wbuf to a fresh row. Used when a glyph would
 * overflow and wbuf began immediately after an emitted space. After
 * replay, later overflow in the same over-long token must char-wrap
 * instead of replaying again, so wbuf_after_space is cleared. */
static void wbuf_replay_on_new_row(struct walker *w)
{
    emit_row_break(w);
    /* Replay buffered glyphs at the new row's starting column. */
    for (size_t i = 0; i < w->wbuf_n; i++)
        emit_glyph(w, &w->wbuf[i]);
    w->wbuf_n = 0;
    w->wbuf_width = 0;
    w->wbuf_after_space = 0;
}

/* The byte index that follows the walker's most recently consumed
 * glyph (whether committed or buffered). Used so an end-of-buffer
 * cursor lands on the correct row/col when no glyph was emitted at
 * cursor's index (cursor == len). */
static void resolve_cursor_at_end(struct walker *w, size_t len)
{
    if (w->cursor_resolved)
        return;
    if (w->cursor >= len) {
        /* Cursor at end-of-buffer: it sits where the next glyph would
         * land — i.e. after any pending wbuf flush. The wbuf flush is
         * the caller's responsibility (input_core_render flushes
         * before resolving). */
        w->cursor_row = w->row;
        w->cursor_col = w->col;
        w->cursor_resolved = 1;
        return;
    }
    /* Cursor mid-buffer but no glyph matched its range (e.g. cursor
     * past the leader of a malformed sequence we substituted as one
     * byte). Fall back to current emit position; matches the old
     * compute_layout behavior. */
    w->cursor_row = w->row;
    w->cursor_col = w->col;
    w->cursor_resolved = 1;
}

void input_core_render(const char *buf, size_t len, size_t cursor, int prompt_w,
                       int cont_indent_col, int cols, input_render_cb cb, void *user,
                       struct input_layout *out)
{
    struct walker w = {.prompt_w = prompt_w,
                       .cont_indent = cont_indent_col,
                       .cols = cols,
                       .cursor = cursor,
                       .cb = cb,
                       .user = user,
                       .row = 0,
                       .col = prompt_w};

    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\n') {
            wbuf_flush(&w);
            /* Cursor exactly on the '\n': it visually sits at the
             * end of the current row (just after the trailing
             * content), matching the legacy compute_layout's
             * "cursor on \n stays at row, col after content"
             * behavior. */
            if (!w.cursor_resolved && w.cursor == i) {
                w.cursor_row = w.row;
                w.cursor_col = w.col;
                w.cursor_resolved = 1;
            }
            emit_row_break(&w);
            i++;
            continue;
        }

        struct walk_glyph g;
        if (!decode_glyph(buf, len, i, &g))
            break;

        /* Combining marks (width 0) ride along on the previous glyph.
         * Append to wbuf without any overflow check — they don't push
         * the column. If wbuf is at capacity, flush it first so the
         * mark emits at the correct visual position (immediately
         * after its host glyph) and its cursor resolution uses the
         * post-flush row/col; emitting the mark before the flush
         * would attach it to whatever glyph last committed before
         * the wbuf fill and put cursor reporting at the wrong column. */
        if (g.width == 0) {
            if (w.wbuf_n < WBUF_CAP) {
                w.wbuf[w.wbuf_n++] = g;
            } else {
                wbuf_flush(&w);
                emit_glyph(&w, &g);
            }
            i += g.consumed;
            continue;
        }

        if (g.is_space) {
            /* Space: commit the trailing word to this row. Then,
             * if the space itself overflows the row, drop it as
             * the wrap point and break — leaves the new row clean
             * of a leading space. Otherwise emit it normally so it
             * separates this word from the next. */
            int over = (w.cols > 0 && w.col + w.wbuf_width + g.width >= w.cols);
            wbuf_flush(&w);
            if (over && w.col > w.cont_indent) {
                if (!w.cursor_resolved && cursor_in_range(w.cursor, g.byte_index, g.consumed)) {
                    w.cursor_row = w.row;
                    w.cursor_col = w.col;
                    w.cursor_resolved = 1;
                }
                emit_row_break(&w);
            } else {
                emit_glyph(&w, &g);
                w.wbuf_after_space = 1;
            }
            i += g.consumed;
            continue;
        }

        /* Non-space glyph. */
        int prospective = w.col + w.wbuf_width + g.width;
        if (w.cols > 0 && prospective >= w.cols) {
            if (w.wbuf_n > 0 && w.wbuf_after_space && w.col > w.cont_indent) {
                /* Buffer holds the trailing word after a real word
                 * boundary; move it to a fresh row. `w.col >
                 * w.cont_indent` guards an empty-row replay. */
                wbuf_replay_on_new_row(&w);
            } else {
                /* No earlier word boundary on this row, or we're
                 * already at row start with an over-long token in
                 * the buffer. Flush the buffer to commit what fit,
                 * then break before the current glyph. */
                wbuf_flush(&w);
                if (w.col > w.cont_indent)
                    emit_row_break(&w);
            }
        }

        if (w.wbuf_n < WBUF_CAP) {
            w.wbuf[w.wbuf_n++] = g;
            w.wbuf_width += g.width;
        } else {
            /* Token longer than wbuf budget — degrade to char-wrap.
             * Flush, break if needed, then emit directly. */
            wbuf_flush(&w);
            if (w.cols > 0 && w.col + g.width >= w.cols && w.col > w.cont_indent)
                emit_row_break(&w);
            emit_glyph(&w, &g);
        }
        i += g.consumed;
    }

    /* Flush any pending tail. If it overflows, replay onto a new
     * row first — same rule as mid-walk. */
    if (w.wbuf_n > 0 && w.cols > 0 && w.col + w.wbuf_width >= w.cols && w.wbuf_after_space &&
        w.col > w.cont_indent) {
        wbuf_replay_on_new_row(&w);
    } else {
        wbuf_flush(&w);
    }

    resolve_cursor_at_end(&w, len);

    if (out) {
        out->cursor_row = w.cursor_row;
        out->cursor_col = w.cursor_col;
        out->end_row = w.row;
        out->end_col = w.col;
        out->total_rows = w.row + 1;
    }
}

void input_core_compute_layout(const char *buf, size_t len, size_t cursor, int prompt_w, int cols,
                               struct input_layout *out)
{
    input_core_render(buf, len, cursor, prompt_w, prompt_w, cols, NULL, NULL, out);
}

/* ---------------- escape-sequence decoder ---------------- */

/* Read the body of a CSI / SS3 sequence into `seq`: bytes after the
 * leading "ESC [" or "ESC O" up to and including the final byte.
 * Returns the number of bytes captured, or -1 on EOF, overflow, or a
 * runaway non-terminating stream — caller aborts in all three cases.
 *
 * Final-byte detection accepts both the ECMA-48 standard range
 * (0x40-0x7E) and '$' (0x24). The latter is technically an
 * "intermediate byte" in ECMA-48, but rxvt uses it as a final for
 * Shift-modified tilde keys (e.g. Shift+Home = "ESC[7$"); since this
 * function only parses keyboard input, no legitimate keyboard
 * sequence puts '$' mid-payload, so the relaxed termination is safe.
 *
 * On overflow (sequence longer than `seq`) we keep reading and
 * discard the excess bytes, then return -1 once the final byte
 * arrives. Stopping at the first overflow byte would leave the rest
 * of the sequence — including the final — queued in stdin, and the
 * main keystroke loop would then insert those bytes as text.
 *
 * Two hard caps bound the work in pathological cases: the per-byte
 * timeout in the real reader (a stalled sequence can't wedge the
 * prompt past Ctrl-C, since ISIG is off in raw mode), and a total
 * read count limit (a peer that streams non-final bytes inside the
 * timeout window otherwise spins this loop indefinitely). Real
 * keyboard sequences are well under a dozen bytes, so the cap is
 * generous. */
static int read_csi_seq(input_byte_reader read, void *user, char *seq, int cap)
{
    const int MAX_READS = 64;
    int n = 0;
    int overflowed = 0;
    for (int reads = 0; reads < MAX_READS; reads++) {
        int b = read(user);
        if (b < 0)
            return -1;
        if (n + 1 < cap)
            seq[n++] = (char)b;
        else
            overflowed = 1;
        if ((b >= 0x40 && b <= 0x7E) || b == '$') {
            if (overflowed)
                return -1;
            seq[n] = '\0';
            return n;
        }
    }
    return -1;
}

/* Parse the xterm-style modifier parameter from a CSI/SS3 cursor-key
 * payload of the form "1;<mod><final>" (e.g. "1;5D" for Ctrl+Left).
 * Returns the raw modifier value (1 = no modifier, 2 = Shift, 3 = Alt,
 * 5 = Ctrl, ...; encoding is 1 + bitmask with bits Shift=1, Alt=2,
 * Ctrl=4, Meta=8). Returns 1 (no modifier) for any payload that
 * doesn't structurally match, including the unmodified short forms.
 *
 * Tolerates trailing parameters separated by additional semicolons
 * (kitty / xterm in modifyOtherKeys mode emit "1;5;2D" with a key-
 * event-type after the modifier). Only the modifier — the parameter
 * we care about — is parsed; the rest is ignored. */
static int parse_xterm_mod(const char *seq, int n)
{
    if (n < 4 || seq[0] != '1' || seq[1] != ';')
        return 1;
    int v = 0;
    for (int i = 2; i < n - 1; i++) {
        if (seq[i] == ';')
            break;
        if (seq[i] < '0' || seq[i] > '9')
            return 1;
        v = v * 10 + (seq[i] - '0');
        if (v > 99)
            return 1;
    }
    return v ? v : 1;
}

/* True when an xterm modifier value implies word motion for arrow
 * keys: Alt, Ctrl, or Meta is part of the chord (bits 2|4|8 in mod-1).
 * Plain (mod=1) and Shift-only (mod=2) fall through to single-char
 * motion — Shift+arrow has no selection meaning in this editor, so
 * the least-surprising fallback is a normal arrow. */
static int xterm_mod_implies_word(int mod)
{
    if (mod < 1)
        return 0;
    return ((mod - 1) & 0xE) != 0;
}

/* Map a CSI/SS3 cursor-key final byte to an action. Letters outside
 * the cursor-key set (e.g. 'Z' for Shift-Tab, function-key finals)
 * fall through to NONE. */
static enum input_action arrow_to_action(char final, int word)
{
    switch (final) {
    case 'A':
        return INPUT_ACTION_HISTORY_PREV;
    case 'B':
        return INPUT_ACTION_HISTORY_NEXT;
    case 'C':
        return word ? INPUT_ACTION_MOVE_WORD_RIGHT : INPUT_ACTION_MOVE_RIGHT;
    case 'D':
        return word ? INPUT_ACTION_MOVE_WORD_LEFT : INPUT_ACTION_MOVE_LEFT;
    case 'H':
        return INPUT_ACTION_LINE_START;
    case 'F':
        return INPUT_ACTION_LINE_END;
    }
    return INPUT_ACTION_NONE;
}

enum input_action input_core_decode_escape(input_byte_reader read, void *user)
{
    int b = read(user);
    if (b < 0)
        return INPUT_ACTION_NONE; /* bare ESC */

    /* iTerm2's "Esc+" mode for Option emits ESC ESC <CSI/SS3> for
     * Alt-modified cursor keys (kLFT3 = "ESC ESC [ D" etc.). Strip
     * leading ESCs and force word motion on the inner arrow. For
     * bare-ESC + letter (b/f/d/...), `meta` is redundant since those
     * bindings are already meta-defined; the flag is harmless there.
     * The strip cap is paranoia against a peer flooding ESCs within
     * the timeout window — real terminals send at most one extra. */
    int meta = 0;
    for (int strip = 0; b == 0x1b && strip < 4; strip++) {
        b = read(user);
        if (b < 0)
            return INPUT_ACTION_NONE;
        meta = 1;
    }
    if (b == 0x1b)
        return INPUT_ACTION_NONE;

    if (b == '[') {
        char seq[32];
        int n = read_csi_seq(read, user, seq, sizeof(seq));
        if (n < 0)
            return INPUT_ACTION_NONE;

        if (n == 1) {
            /* Unmodified arrow / Home / End. rxvt encodes Shift+arrow
             * here as lowercase a/b/c/d; without selection support
             * the least-surprising fallback is plain single-char
             * motion, so normalize the case and dispatch unmodified. */
            char final = seq[0];
            if (final >= 'a' && final <= 'd')
                final -= 'a' - 'A';
            return arrow_to_action(final, meta);
        }
        if (strcmp(seq, "200~") == 0)
            return INPUT_ACTION_PASTE_BEGIN;

        /* Tilde-key family: Home/End/Delete and friends, encoded as
         * "<digit><final>" with optional modifier. xterm uses '~' and
         * "<digit>;<mod>~" for modified; rxvt encodes the modifier in
         * the final byte itself ('~' = none, '$' = Shift, '^' = Ctrl,
         * '@' = Ctrl+Shift). Modifier doesn't change the action for
         * the keys we care about, so the final byte is treated
         * uniformly.
         *
         * The code prefix must be a single digit — multi-digit codes
         * (function keys F5+: "15~", "17~", ...) deliberately fall
         * through, otherwise their leading digit would alias to
         * Home/End/Delete. Accept the unmodified short form ("3~")
         * and the xterm modified form ("3;5~"). */
        char final = seq[n - 1];
        if ((final == '~' || final == '^' || final == '$' || final == '@') &&
            (n == 2 || (n >= 4 && seq[1] == ';'))) {
            switch (seq[0]) {
            case '1':
            case '7':
                return INPUT_ACTION_LINE_START;
            case '4':
            case '8':
                return INPUT_ACTION_LINE_END;
            case '3':
                return INPUT_ACTION_DELETE_FWD;
            }
        }
        /* Modified arrows / Home / End: "1;<mod><letter>". Modifier
         * value is decoded; only Alt/Ctrl/Meta-flavored arrows take
         * the word-motion path, while Shift-only and unmodified fall
         * back to single-character motion. Home/End ignore the
         * modifier. */
        if (n >= 4 && seq[0] == '1' && seq[1] == ';') {
            int mod = parse_xterm_mod(seq, n);
            return arrow_to_action(seq[n - 1], xterm_mod_implies_word(mod) || meta);
        }
        return INPUT_ACTION_NONE;
    }

    if (b == 'O') {
        /* SS3 cursor keys, used by some terminals in application-
         * cursor mode. Three flavors:
         *   - unmodified: "ESC O <A|B|C|D|H|F>"
         *   - xterm modified: "ESC O 1;<mod><letter>"
         *   - rxvt Ctrl+arrow: "ESC O <a|b|c|d>" (lowercase final)
         * Reading to the final byte handles all three and drains any
         * unrecognized SS3 payload (function keys F1-F4) cleanly so
         * leftover bytes don't leak into the keystroke loop as text. */
        char seq[32];
        int n = read_csi_seq(read, user, seq, sizeof(seq));
        if (n < 0)
            return INPUT_ACTION_NONE;
        char final = seq[n - 1];
        int word;
        if (final >= 'a' && final <= 'd') {
            final -= 'a' - 'A';
            word = 1;
        } else {
            word = xterm_mod_implies_word(parse_xterm_mod(seq, n));
        }
        return arrow_to_action(final, word || meta);
    }

    /* Alt+Enter (ESC + CR/LF) inserts a newline, for terminals that
     * don't deliver Shift+Enter as a bare LF. */
    if (b == '\r' || b == '\n')
        return INPUT_ACTION_INSERT_NEWLINE;

    /* Readline-style Meta bindings — also what macOS Terminal sends
     * for Option+Left/Right when "Use Option as Meta key" is enabled,
     * doubling as Alt+arrow handling on terminals that don't emit
     * CSI modified arrows. */
    switch (b) {
    case 'b':
    case 'B':
        return INPUT_ACTION_MOVE_WORD_LEFT;
    case 'f':
    case 'F':
        return INPUT_ACTION_MOVE_WORD_RIGHT;
    case 'd':
    case 'D':
        return INPUT_ACTION_KILL_WORD_FWD;
    case 0x7f: /* Alt+Backspace (most terminals) */
    case 0x08: /* Alt+Backspace (terminals that map Backspace to ^H) */
        return INPUT_ACTION_KILL_WORD_BACK_ALNUM;
    }
    return INPUT_ACTION_NONE;
}
