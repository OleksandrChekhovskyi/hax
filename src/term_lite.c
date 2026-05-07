/* SPDX-License-Identifier: MIT */
#include "term_lite.h"

#include <string.h>

#include "util.h"

/* Ring access helpers. Logical row index → physical slot in rows[]. */
static struct buf *row_at(struct term_lite *t, size_t logical)
{
    return &t->rows[(t->head + logical) % TERM_LITE_RING_CAP];
}

static unsigned char *hn_at(struct term_lite *t, size_t logical)
{
    return &t->has_newline[(t->head + logical) % TERM_LITE_RING_CAP];
}

/* Track cursor extremes for the windowed-redraw depth metric.
 * Called after every cur_row mutation. The first time cur_row dips
 * below global_high_water, latch cursor_moved_up and start tracking
 * tracked_min as the deepest cursor position seen. Until that latch,
 * tracked_min follows cur_row (handling the "stream just started,
 * no dip yet" case so depth reads as 0 for pure-linear). After the
 * latch, tracked_min only ever decreases — it's the deepest reach
 * the producer's CUU has done, which bounds the window depth. */
static void note_cursor_move(struct term_lite *t)
{
    if (t->cur_row > t->global_high_water)
        t->global_high_water = t->cur_row;
    if (t->cur_row < t->global_high_water) {
        if (!t->cursor_moved_up) {
            t->cursor_moved_up = 1;
            t->tracked_min = t->cur_row;
        } else if (t->cur_row < t->tracked_min) {
            t->tracked_min = t->cur_row;
        }
    } else if (!t->cursor_moved_up) {
        /* Pre-latch: tracked_min follows cur_row (which equals the
         * high-water at this point) so a stream that never dips
         * reads as depth=0. */
        t->tracked_min = t->cur_row;
    }
}

void term_lite_init(struct term_lite *t)
{
    memset(t, 0, sizeof(*t));
    /* Always start with one empty row so cur_row=0 points somewhere. */
    t->count = 1;
}

void term_lite_free(struct term_lite *t)
{
    for (size_t i = 0; i < TERM_LITE_RING_CAP; i++)
        buf_free(&t->rows[i]);
}

/* Make logical row `target` exist (extend `count` if needed, evicting
 * oldest rows from the ring head when full). Returns the (possibly
 * shifted-down) target row index — eviction renumbers logical rows,
 * so a caller asking for row R may get back R-1, R-2, … if rows fell
 * off the head during the call. cur_row is similarly adjusted in place. */
static size_t ensure_row(struct term_lite *t, size_t target, term_lite_line_fn on_line, void *user)
{
    while (target >= t->count) {
        if (t->count == TERM_LITE_RING_CAP) {
            /* Evict the head row — commits via on_line, then drops it
             * from the ring. cur_row and target both shift down by 1
             * so they keep pointing at the same physical row. The
             * dropped slot's touched bits clear so a future row that
             * lands in this slot doesn't inherit stale activity. */
            struct buf *b = &t->rows[t->head];
            if (on_line)
                on_line(b->data ? b->data : "", b->len, t->has_newline[t->head], user);
            buf_reset(b);
            t->has_newline[t->head] = 0;
            t->head = (t->head + 1) % TERM_LITE_RING_CAP;
            t->count--;
            if (t->cur_row > 0)
                t->cur_row--;
            if (target > 0)
                target--;
            /* Logical indices shift down by 1; bring the cursor
             * extremes trackers along so the depth metric stays
             * meaningful through mid-chunk evictions. */
            if (t->global_high_water > 0)
                t->global_high_water--;
            if (t->tracked_min > 0)
                t->tracked_min--;
        } else {
            /* Append a new empty row at the tail. */
            size_t idx = (t->head + t->count) % TERM_LITE_RING_CAP;
            buf_reset(&t->rows[idx]);
            t->has_newline[idx] = 0;
            t->count++;
        }
    }
    return target;
}

/* Drop the head row from the ring — emit via on_line (skipping empty
 * unterminated trailing rows), buf_reset, advance head. Decrements
 * cur_row alongside since logical indices shift down. */
static void evict_head(struct term_lite *t, term_lite_line_fn on_line, void *user)
{
    size_t slot = t->head;
    struct buf *b = &t->rows[slot];
    int h = t->has_newline[slot];
    if ((b->len > 0 || h) && on_line)
        on_line(b->data ? b->data : "", b->len, h, user);
    buf_reset(b);
    t->has_newline[slot] = 0;
    t->head = (t->head + 1) % TERM_LITE_RING_CAP;
    t->count--;
    if (t->cur_row > 0)
        t->cur_row--;
}

/* Margin-based settle: emit head rows until cur_row drops to
 * max_cuu_distance. Rows older than the deepest observed CUU depth
 * can't be reached by a future cursor-up, so they're safe to commit.
 * For pure-linear streams (no CUU ever), max_cuu_distance is 0 and
 * this settles every row that's strictly behind the cursor — i.e.,
 * every \n that completes a row commits it on the same feed call.
 * For windowed streams, max_cuu_distance grows with observed window
 * depth, keeping the active redraw region buffered. */
static void settle(struct term_lite *t, term_lite_line_fn on_line, void *user)
{
    while (t->cur_row > t->max_cuu_distance)
        evict_head(t, on_line, user);
}

/* Move the cursor back by one codepoint (UTF-8 aware). The row buffer
 * itself is not mutated — real \b just moves the cursor; subsequent
 * writes overwrite. No-op when already at column 0. */
static void cursor_back_codepoint(struct term_lite *t)
{
    if (t->cur_col == 0)
        return;
    struct buf *row = row_at(t, t->cur_row);
    size_t c = t->cur_col;
    while (c > 0) {
        c--;
        if (c >= row->len) {
            /* cur_col was past row data (CUU landed cursor at a wide
             * col on a shorter row). Treat as ASCII — single step back. */
            t->cur_col = c;
            return;
        }
        unsigned char b = (unsigned char)row->data[c];
        if (b < 0x80 || b >= 0xC0) {
            /* Lead byte of a codepoint — done. */
            t->cur_col = c;
            return;
        }
        /* Continuation byte (0x80-0xBF): keep walking. */
    }
    t->cur_col = 0;
}

/* Write a single byte at the cursor. Three cases:
 *   - cur_col == row.len: append, extending the row.
 *   - cur_col < row.len:  overwrite the byte at cur_col.
 *   - cur_col > row.len:  pad with spaces up to cur_col, then append.
 *                         (Happens when CUU lands cursor at a wide col
 *                         on a shorter row.) */
static void write_byte(struct term_lite *t, unsigned char c)
{
    struct buf *row = row_at(t, t->cur_row);
    if (t->cur_col == row->len) {
        buf_append(row, &c, 1);
    } else if (t->cur_col < row->len) {
        row->data[t->cur_col] = (char)c;
    } else {
        while (row->len < t->cur_col)
            buf_append(row, " ", 1);
        buf_append(row, &c, 1);
    }
    t->cur_col++;
}

/* Parse the n-th decimal parameter from a CSI param/intermediate buffer.
 * CSI param syntax: digit-runs separated by ';'. Missing param == 0.
 * Many CSI ops then treat 0 as "default 1" — that mapping happens in
 * apply_csi, not here. */
static int csi_param(const char *buf, size_t len, size_t n)
{
    size_t pos = 0;
    size_t idx = 0;
    int val = 0;
    int has_digit = 0;
    while (pos <= len) {
        char c = (pos < len) ? buf[pos] : ';';
        if (c == ';') {
            if (idx == n)
                return has_digit ? val : 0;
            idx++;
            val = 0;
            has_digit = 0;
        } else if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            has_digit = 1;
        }
        /* Other bytes (intermediates 0x20-0x2F, '?' modifier 0x3F) are
         * silently ignored — we don't model private modes. */
        pos++;
    }
    return 0;
}

/* Erase line variants. n=0: cursor → EOL; n=1: BOL → cursor (filled
 * with spaces); n=2: whole row. Real terminals erase to spaces, not
 * truncate; we truncate for n=0 because trailing spaces would survive
 * into the model's transcript and read as visible padding. n=1's
 * behavior preserves the post-cursor content which is what a real
 * terminal does. */
static void erase_line(struct term_lite *t, int n)
{
    struct buf *row = row_at(t, t->cur_row);
    if (n == 0) {
        if (t->cur_col < row->len)
            row->len = t->cur_col;
    } else if (n == 1) {
        if (row->len < t->cur_col) {
            while (row->len < t->cur_col)
                buf_append(row, " ", 1);
        } else {
            for (size_t i = 0; i < t->cur_col; i++)
                row->data[i] = ' ';
        }
    } else if (n == 2) {
        buf_reset(row);
    }
}

/* Erase display variants. n=0: clear cur row from cursor + truncate
 * rows below. n=1: clear all rows above + cur row from BOL to cursor.
 * n=2: clear every row in the buffer. Truncating rows below means
 * dropping them entirely — they were never \n-terminated and have no
 * content, so flush wouldn't have emitted them anyway. */
static void erase_display(struct term_lite *t, int n)
{
    if (n == 0) {
        struct buf *row = row_at(t, t->cur_row);
        if (t->cur_col < row->len)
            row->len = t->cur_col;
        for (size_t i = t->cur_row + 1; i < t->count; i++) {
            buf_reset(row_at(t, i));
            *hn_at(t, i) = 0;
        }
        t->count = t->cur_row + 1;
    } else if (n == 1) {
        for (size_t i = 0; i < t->cur_row; i++) {
            buf_reset(row_at(t, i));
            *hn_at(t, i) = 0;
        }
        struct buf *row = row_at(t, t->cur_row);
        if (row->len < t->cur_col) {
            while (row->len < t->cur_col)
                buf_append(row, " ", 1);
        } else {
            for (size_t i = 0; i < t->cur_col; i++)
                row->data[i] = ' ';
        }
    } else if (n == 2) {
        for (size_t i = 0; i < t->count; i++) {
            buf_reset(row_at(t, i));
            *hn_at(t, i) = 0;
        }
    }
}

/* Apply a parsed CSI sequence (final byte already validated against
 * ctrl_strip's allowlist). Missing/zero parameters default to 1 for
 * the ECMA-48 ops that take a count; H/f have separate row+col
 * defaults. */
static void apply_csi(struct term_lite *t, char final, term_lite_line_fn on_line, void *user)
{
    int n = csi_param(t->esc_buf, t->esc_len, 0);

    /* Snapshot the cursor's slot so we can detect whether this op
     * physically moved cursor to a different row. If it did, the row
     * we leave should read as "completed" in the model output —
     * otherwise CNL/CUP/VPA/etc. on a content-bearing row would emit
     * it as a trailing partial (no \n separator), gluing it onto the
     * next row. Tracked by ring slot rather than logical index so
     * eviction during ensure_row doesn't fool us. */
    size_t old_slot = (t->head + t->cur_row) % TERM_LITE_RING_CAP;

    if (final == 'A') {
        if (n == 0)
            n = 1;
        size_t actual = (size_t)n > t->cur_row ? t->cur_row : (size_t)n;
        t->cur_row -= actual;
        /* CUU never crosses col 0 — cursor stays in the same column.
         * Track the actual distance moved (post-clamp) so the margin
         * reflects how far up cursor really went. */
        note_cursor_move(t);
    } else if (final == 'B') {
        if (n == 0)
            n = 1;
        if ((size_t)n >= TERM_LITE_RING_CAP)
            n = TERM_LITE_RING_CAP - 1;
        size_t target = t->cur_row + (size_t)n;
        target = ensure_row(t, target, on_line, user);
        t->cur_row = target;
        note_cursor_move(t);
    } else if (final == 'C') {
        /* CUF — Cursor Forward (column +). cur_col can land past
         * row.len; write_byte pads with spaces in that case. */
        if (n == 0)
            n = 1;
        t->cur_col += (size_t)n;
    } else if (final == 'D') {
        /* CUB — Cursor Back. Clamps at column 0. */
        if (n == 0)
            n = 1;
        if ((size_t)n > t->cur_col)
            t->cur_col = 0;
        else
            t->cur_col -= (size_t)n;
    } else if (final == 'E') {
        /* CNL — Cursor Next Line. Down by n, col reset to 0. */
        if (n == 0)
            n = 1;
        if ((size_t)n >= TERM_LITE_RING_CAP)
            n = TERM_LITE_RING_CAP - 1;
        size_t target = t->cur_row + (size_t)n;
        target = ensure_row(t, target, on_line, user);
        t->cur_row = target;
        t->cur_col = 0;
        note_cursor_move(t);
    } else if (final == 'F') {
        /* CPL — Cursor Previous Line. Up by n, col reset to 0.
         * Same windowed-redraw signal as CUU. */
        if (n == 0)
            n = 1;
        size_t actual = (size_t)n > t->cur_row ? t->cur_row : (size_t)n;
        t->cur_row -= actual;
        t->cur_col = 0;
        note_cursor_move(t);
    } else if (final == 'G') {
        /* CHA — Cursor Horizontal Absolute. Param is 1-indexed column;
         * default 1. write_byte pads with spaces if cur_col lands past
         * the current row's end, so it's safe to set arbitrarily. */
        if (n == 0)
            n = 1;
        t->cur_col = (size_t)(n - 1);
    } else if (final == 'H' || final == 'f') {
        /* CUP / HVP — set row;col absolute, both 1-indexed. Missing
         * params default to 1. Row is capped at ring capacity to
         * bound work for pathological "CUP to row 1M" inputs. */
        int row = csi_param(t->esc_buf, t->esc_len, 0);
        int col = csi_param(t->esc_buf, t->esc_len, 1);
        if (row == 0)
            row = 1;
        if (col == 0)
            col = 1;
        size_t target = (size_t)(row - 1);
        if (target >= TERM_LITE_RING_CAP)
            target = TERM_LITE_RING_CAP - 1;
        target = ensure_row(t, target, on_line, user);
        t->cur_row = target;
        t->cur_col = (size_t)(col - 1);
        note_cursor_move(t);
    } else if (final == 'd') {
        /* VPA — Vertical Position Absolute. Same shape as CHA but
         * for the row axis. Capped at ring capacity. */
        if (n == 0)
            n = 1;
        size_t target = (size_t)(n - 1);
        if (target >= TERM_LITE_RING_CAP)
            target = TERM_LITE_RING_CAP - 1;
        target = ensure_row(t, target, on_line, user);
        t->cur_row = target;
        note_cursor_move(t);
    } else if (final == 's') {
        /* SCOSC — save cursor position. Stored as the logical row
         * index; eviction between save and restore degrades gracefully
         * via clamping in 'u'. */
        t->saved_row = t->cur_row;
        t->saved_col = t->cur_col;
        t->saved_valid = 1;
    } else if (final == 'u') {
        /* SCORC — restore cursor position. If the saved row was
         * evicted (saved_row >= count after eviction) clamp to the
         * last still-buffered row. If no save ever fired, treat as
         * "go to home" (row 0 col 0). */
        if (!t->saved_valid) {
            t->cur_row = 0;
            t->cur_col = 0;
        } else {
            t->cur_row = t->saved_row < t->count ? t->saved_row : t->count - 1;
            t->cur_col = t->saved_col;
        }
        note_cursor_move(t);
    } else if (final == 'K') {
        erase_line(t, n);
    } else if (final == 'J') {
        erase_display(t, n);
    }
    /* Unknown finals shouldn't reach us — ctrl_strip's allowlist gates
     * this. Silently ignore if anyone does feed one in directly. */

    size_t new_slot = (t->head + t->cur_row) % TERM_LITE_RING_CAP;
    if (new_slot != old_slot) {
        /* Old slot still in the ring (eviction during a deep CUD
         * could have dropped it)? Logical index of old_slot from the
         * current head: distance modulo CAP, must be < count. */
        size_t logical = (old_slot + TERM_LITE_RING_CAP - t->head) % TERM_LITE_RING_CAP;
        if (logical < t->count && t->rows[old_slot].len > 0)
            t->has_newline[old_slot] = 1;
    }
}

void term_lite_feed(struct term_lite *t, const char *bytes, size_t n, term_lite_line_fn on_line,
                    void *user)
{
    if (n == 0)
        return;
    /* Bootstrap cursor extremes for the first feed: bring the
     * high-water/tracked-min trackers up to the current cur_row
     * (e.g., after init cur_row=0). After the first byte they
     * maintain themselves via note_cursor_move. */
    if (t->cur_row > t->global_high_water)
        t->global_high_water = t->cur_row;
    if (!t->cursor_moved_up && t->tracked_min < t->cur_row)
        t->tracked_min = t->cur_row;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];

        /* CSI accumulator state — we're between "ESC [" and the final
         * byte. ECMA-48 says any 0x40-0x7E ends the sequence; LF / CAN
         * / SUB cancel it. Bytes outside [0x20, 0x3F] aren't supposed
         * to appear inside a CSI but we ignore them defensively. */
        if (t->esc_state == '[') {
            if (c == 0x0a || c == 0x18 || c == 0x1a) {
                t->esc_state = 0;
                t->esc_len = 0;
                /* Reconsume the cancelling byte under the normal arm
                 * so a real LF still advances the cursor. */
                i--;
                continue;
            }
            if (c >= 0x40 && c <= 0x7e) {
                apply_csi(t, (char)c, on_line, user);
                t->esc_state = 0;
                t->esc_len = 0;
                continue;
            }
            if (t->esc_len < sizeof(t->esc_buf))
                t->esc_buf[t->esc_len++] = (char)c;
            /* Buffer overflow → silently drop the byte. The CSI will
             * still apply when its final arrives, just with truncated
             * params. Real CSIs never approach this length. */
            continue;
        }
        if (t->esc_state == 'E') {
            if (c == '[') {
                t->esc_state = '[';
                t->esc_len = 0;
            } else {
                /* ctrl_strip should never forward a non-CSI ESC. If one
                 * leaks through, drop the ESC and reconsume the byte. */
                t->esc_state = 0;
                i--;
            }
            continue;
        }

        if (c == 0x1b) {
            t->esc_state = 'E';
            continue;
        }
        if (c == '\n') {
            *hn_at(t, t->cur_row) = 1;
            size_t target = t->cur_row + 1;
            target = ensure_row(t, target, on_line, user);
            t->cur_row = target;
            t->cur_col = 0;
            note_cursor_move(t);
            continue;
        }
        if (c == '\r') {
            t->cur_col = 0;
            continue;
        }
        if (c == '\b') {
            cursor_back_codepoint(t);
            continue;
        }
        write_byte(t, c);
    }

    /* Recompute max_cuu_distance from the cumulative high-water and
     * deepest-min trackers. Captures both [1A distance and any
     * forward-write extension that pushed the cursor past the old
     * high-water (windowHeight grew mid-stream). max_cuu_distance is
     * monotone non-decreasing — once we've seen depth N, we keep
     * the buffer big enough for it forever. */
    if (t->cursor_moved_up) {
        size_t depth = t->global_high_water - t->tracked_min;
        if (depth > t->max_cuu_distance)
            t->max_cuu_distance = depth;
    }

    /* Graduate rows that are safely past the cursor — same policy
     * for pure-linear and windowed streams. max_cuu_distance is 0
     * for pure-linear, so settle commits every row strictly behind
     * cur_row (each \n graduates its row immediately). Windowed
     * streams have max_cuu_distance > 0, keeping the redraw region
     * buffered while older rows still commit. */
    settle(t, on_line, user);
}

void term_lite_flush(struct term_lite *t, term_lite_line_fn on_line, void *user)
{
    for (size_t i = 0; i < t->count; i++) {
        struct buf *r = row_at(t, i);
        int h = t->has_newline[(t->head + i) % TERM_LITE_RING_CAP];
        /* Skip rows that were never written and never \n-terminated:
         * the initial empty row before any feed, the row the cursor
         * lands on after a stream-ending \n, rows allocated by CUD
         * but never touched. has_newline == 1 with len == 0 is a
         * deliberate blank line — preserve it. */
        if (r->len == 0 && !h)
            continue;
        if (on_line)
            on_line(r->data ? r->data : "", r->len, h, user);
    }
    /* Reset to clean post-init state. */
    for (size_t i = 0; i < TERM_LITE_RING_CAP; i++) {
        buf_reset(&t->rows[i]);
        t->has_newline[i] = 0;
    }
    t->head = 0;
    t->count = 1;
    t->cur_row = 0;
    t->cur_col = 0;
    t->esc_state = 0;
    t->esc_len = 0;
    t->saved_row = 0;
    t->saved_col = 0;
    t->saved_valid = 0;
    t->cursor_moved_up = 0;
    t->global_high_water = 0;
    t->tracked_min = 0;
    t->max_cuu_distance = 0;
}

const char *term_lite_cur_data(const struct term_lite *t)
{
    const struct buf *row = &t->rows[(t->head + t->cur_row) % TERM_LITE_RING_CAP];
    return row->data ? row->data : "";
}

size_t term_lite_cur_len(const struct term_lite *t)
{
    const struct buf *row = &t->rows[(t->head + t->cur_row) % TERM_LITE_RING_CAP];
    return row->len;
}

int term_lite_cur_is_blank(const struct term_lite *t)
{
    size_t len = term_lite_cur_len(t);
    const char *data = term_lite_cur_data(t);
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c != ' ' && c != '\t')
            return 0;
    }
    return 1;
}

/* Bottom-row accessors. count is always ≥ 1 after init, so logical row
 * (count - 1) is always valid. Used by tool_render's spinner label so
 * the live status pins to the highest-touched row instead of cur_row,
 * which jumps around the redraw window for tools like vitest and
 * causes the spinner label to strobe at sub-frame rates. */
const char *term_lite_bottom_data(const struct term_lite *t)
{
    const struct buf *row = &t->rows[(t->head + t->count - 1) % TERM_LITE_RING_CAP];
    return row->data ? row->data : "";
}

size_t term_lite_bottom_len(const struct term_lite *t)
{
    const struct buf *row = &t->rows[(t->head + t->count - 1) % TERM_LITE_RING_CAP];
    return row->len;
}

int term_lite_bottom_is_blank(const struct term_lite *t)
{
    size_t len = term_lite_bottom_len(t);
    const char *data = term_lite_bottom_data(t);
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c != ' ' && c != '\t')
            return 0;
    }
    return 1;
}

/* "Active" row: walk from the bottom upward and return the first row
 * with non-whitespace content. Best signal for the spinner label —
 * skips the trailing empty row left by a \n advance, and ignores
 * cleared-but-not-yet-rewritten rows during multi-row redraws. Falls
 * back to the bottom row if nothing has visible content (so the
 * accessor is always defined). */
static size_t active_row_logical(const struct term_lite *t)
{
    for (size_t i = t->count; i > 0; i--) {
        size_t r = i - 1;
        const struct buf *row = &t->rows[(t->head + r) % TERM_LITE_RING_CAP];
        for (size_t j = 0; j < row->len; j++) {
            char c = row->data[j];
            if (c != ' ' && c != '\t')
                return r;
        }
    }
    return t->count > 0 ? t->count - 1 : 0;
}

const char *term_lite_active_data(const struct term_lite *t)
{
    size_t r = active_row_logical(t);
    const struct buf *row = &t->rows[(t->head + r) % TERM_LITE_RING_CAP];
    return row->data ? row->data : "";
}

size_t term_lite_active_len(const struct term_lite *t)
{
    size_t r = active_row_logical(t);
    return t->rows[(t->head + r) % TERM_LITE_RING_CAP].len;
}

int term_lite_active_is_blank(const struct term_lite *t)
{
    size_t len = term_lite_active_len(t);
    const char *data = term_lite_active_data(t);
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c != ' ' && c != '\t')
            return 0;
    }
    return 1;
}
