/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERM_LITE_H
#define HAX_TERM_LITE_H

#include <stddef.h>
#include <stdint.h>

#include "util.h" /* struct buf */

/*
 * Small bounded subset of a tty for interpreting tool output.
 *
 * Real terminal-bound programs (ninja, meson, cargo, npm, pip, vitest)
 * paint into a tty using a tiny vocabulary of control bytes — carriage
 * returns to overprint a single line, backspace for in-place edits,
 * cursor-up + erase-line to repaint a multi-row "window" at the bottom
 * of the screen. ctrl_strip upstream drops the bytes that don't matter
 * for layout (SGR colors, OSC titles, alternate-charset shifts, …) and
 * forwards a small allowlist of layout-affecting CSIs (CUU/CUD/EL/ED)
 * along with the C0 controls (\r, \b, \n) so this module can interpret
 * them the way a tty would. The output is one clean "committed line"
 * event per row that scrolls past the buffered region, plus accessors
 * for the row at the cursor so the live preview can peek at what's
 * being painted right now.
 *
 * "Lite" because we model just enough to keep the model's transcript
 * matching what a user would have seen — no SGR state, no wcwidth-aware
 * columns, no scroll regions, no alternate screen, no absolute cursor
 * positioning. Cursor column is a byte offset into the row buffer; cell
 * widths beyond ASCII aren't tracked. Acceptable today because the tools
 * that drive multi-row redraws stick to CR + CUU + EL, none of which
 * need cell-width awareness.
 *
 * Implemented:
 *   - \r:        cursor_col = 0 (no row-wide clear; subsequent writes
 *                overwrite byte-by-byte, real-terminal style).
 *   - \b:        cursor_col -= 1 codepoint (UTF-8 aware, no row mutation).
 *                Real \b moves the cursor only; the next write overwrites.
 *   - \n:        mark current row as "had newline", advance to next row,
 *                cursor_col = 0. If advancing past the buffered ring's
 *                tail, the oldest row evicts via on_line.
 *   - other byte: write at (cur_row, cur_col), overwriting existing byte
 *                at that column or appending if at end. Pads with spaces
 *                if cur_col is past the row's current end.
 *   - CSI A (CUU): cursor_row -= n, clamped at 0. Older rows that have
 *                already evicted off the ring are unrecoverable.
 *   - CSI B (CUD): cursor_row += n, allocating new rows (and evicting
 *                from the ring head as needed).
 *   - CSI C/D (CUF/CUB): cur_col +/- n. CUB clamps at 0; CUF can land
 *                cursor past row.len, in which case the next write_byte
 *                pads with spaces.
 *   - CSI E/F (CNL/CPL): cursor down/up by n, col reset to 0.
 *   - CSI G (CHA): cur_col = n - 1 (1-indexed → 0-indexed). Used by
 *                progress-spinner libs to "<glyph> ESC[1G ESC[K" the
 *                final frame off the row.
 *   - CSI H/f (CUP/HVP): cursor to row;col absolute (both 1-indexed).
 *                Row clamped to ring capacity to bound work for a
 *                misbehaving "CUP to row 1M" input.
 *   - CSI J (ED): erase display. n=0 truncates current row from cur_col
 *                + clears all rows below cursor; n=1 clears all rows
 *                above + fills 0..cur_col with spaces; n=2 clears all
 *                rows in the buffer.
 *   - CSI K (EL): erase line. n=0 truncates from cur_col to end (default);
 *                n=1 fills 0..cur_col with spaces; n=2 clears whole row.
 *   - CSI d (VPA): cur_row = n - 1 (1-indexed). Counterpart to CHA.
 *   - CSI s/u (SCOSC/SCORC): save / restore cursor position. Saved
 *                position is the (logical_row, col) at save time; if
 *                the row was evicted from the ring before restore, we
 *                clamp to the closest still-buffered row.
 *
 * Buffered ring capacity (TERM_LITE_RING_CAP) is sized generously enough
 * to absorb typical multi-row redraw windows (vitest reporters,
 * cargo-style progress) without committing intermediate states. Beyond
 * the cap, oldest rows commit via on_line — so a long-running stream
 * with no cursor-up still flushes lines in order, just with a small
 * latency.
 *
 * Stateful across feed() calls — chunked input, including CSI sequences
 * split across chunks, is handled. Callbacks fire synchronously from
 * feed() (during eviction or per-chunk settle) and flush() (for
 * everything still buffered).
 *
 * Per-chunk settle policy: a row is "settled" (emitted via on_line and
 * dropped from the ring) when it was active in the *previous* feed()
 * call but not the current one — i.e., the tool stopped repainting it.
 * This keeps the dim block ticking forward in real time for tools that
 * stream lines (ninja, log output) while leaving multi-row redraw
 * windows (vitest, cargo) buffered until they finish or scroll off.
 * Big single-chunk inputs don't settle mid-chunk — flush handles them.
 */

/* Cap on rows held in the ring before eviction. Generous enough for
 * vitest-style multi-row redraws (typically 6-12 rows) plus headroom
 * for log lines that scroll above the window without forcing premature
 * commits. Memory: ~64 × sizeof(struct buf) of metadata, plus whatever
 * the row buffers grow to (typically <200 bytes each). */
#define TERM_LITE_RING_CAP 64

struct term_lite {
    /* Ring buffer of rows. Logical row L (0 ≤ L < count) lives at
     * rows[(head + L) % CAP]. Older rows than `head` have been
     * evicted via on_line. Always count ≥ 1 after init — the cursor
     * always has a row to point at, even when it's empty. */
    struct buf rows[TERM_LITE_RING_CAP];
    /* Per-row flag: 1 if the row was terminated by \n in the input
     * stream (i.e., cursor at some point advanced past it via \n).
     * Drives the has_newline argument passed to on_line so callers
     * can distinguish complete rows from a true trailing partial. */
    unsigned char has_newline[TERM_LITE_RING_CAP];
    size_t head;    /* ring index of logical row 0 */
    size_t count;   /* alive rows, 1 ≤ count ≤ CAP */
    size_t cur_row; /* logical, 0 ≤ cur_row < count */
    size_t cur_col; /* byte offset into the cursor row */

    /* CSI parser state. esc_state values:
     *   0   = normal stream
     *   'E' = saw ESC (0x1B), awaiting next byte
     *   '[' = inside CSI, accumulating params/intermediates
     * esc_buf accumulates the bytes between "ESC [" and the final;
     * params + intermediates rarely exceed 8 bytes, 32 is plenty. */
    int esc_state;
    char esc_buf[32];
    size_t esc_len;

    /* Cursor-up tracking for windowed redraws.
     *
     * cursor_moved_up latches the first time cur_row goes below the
     * global high-water mark. That signals windowed-redraw mode and
     * switches the settle policy from per-chunk (sound only for
     * strictly-forward streams like ninja or pure log output) to
     * margin-based (keep cur_row + max_cuu_distance rows so the
     * producer can revisit them via CUU).
     *
     * tracked_min is the deepest cur_row observed since the latch
     * fired — cumulative across chunks, never resets except on flush.
     * max_cuu_distance derives from (global_high_water - tracked_min)
     * at end-of-chunk, capturing both the [1A distance AND any
     * forward-write extension that pushed the cursor past the old
     * high-water. Critical when the window GROWS mid-stream: vitest's
     * clearWindow uses the old windowHeight for [1A's, then writes
     * the new (larger) window with extra \n's that push cur_row past
     * the old high-water. Tracking only the [1A count would miss
     * those extra rows and let the new window's top settle into the
     * model as a duplicate frame. */
    size_t global_high_water;
    size_t tracked_min;
    size_t max_cuu_distance;
    int cursor_moved_up;

    /* SCOSC (CSI s) saves these; SCORC (CSI u) restores them. We save
     * the *logical* row index, not a ring slot — if rows evict between
     * save and restore the index is clamped to the new count. */
    size_t saved_row;
    size_t saved_col;
    int saved_valid; /* 1 once SCOSC has fired at least once */
};

/* on_line fires once per row that exits the buffer — either by
 * eviction during feed (oldest row falls off the ring's head) or by
 * flush (every remaining row drained). has_newline distinguishes:
 *   1: row was followed by \n in the input (cursor advanced past it).
 *      Caller typically appends "\n" to reconstruct the original.
 *   0: row is a true trailing partial (cursor sat on it without ever
 *      writing \n). Caller should NOT add a newline.
 * len = 0 && has_newline = 1 is a deliberate blank line and should
 * be preserved; len = 0 && has_newline = 0 is suppressed by flush. */
typedef void (*term_lite_line_fn)(const char *line, size_t len, int has_newline, void *user);

void term_lite_init(struct term_lite *t);
void term_lite_free(struct term_lite *t);

/* Feed n bytes through the terminal interpreter. on_line fires per
 * row that gets evicted from the ring. The cursor row and any rows
 * still in the ring stay buffered until flush. Pass NULL for on_line
 * if you only care about the cursor row content (live-preview path). */
void term_lite_feed(struct term_lite *t, const char *bytes, size_t n, term_lite_line_fn on_line,
                    void *user);

/* Drain every remaining row through on_line and reset state. Each row
 * gets its accumulated has_newline. Empty rows that never received \n
 * are skipped (suppresses the no-op trailing row that follows a
 * stream-ending \n, and the initial empty row if no bytes were fed). */
void term_lite_flush(struct term_lite *t, term_lite_line_fn on_line, void *user);

/* Live-preview accessors. Three flavors:
 *
 *   cur_*    — the row the cursor is currently on. Useful when you
 *              want to know exactly what byte position the producer
 *              is editing right now.
 *   bottom_* — the *last* (highest-logical-index) row in the ring.
 *              Often empty after a \n advance leaves cursor on a
 *              fresh row.
 *   active_* — the last *non-empty* row, walking from the bottom up.
 *              Best signal for "what is the producer painting?" —
 *              skips the empty trailing row that follows a \n advance,
 *              and avoids the cursor-flicker that plain `cur` shows
 *              during multi-row redraws (vitest's clearWindow walks
 *              cursor up through cleared rows; active stays pinned
 *              to whatever was most recently written). Use this for
 *              the spinner label.
 *
 * All three return pointers into the internal buffer — valid only
 * until the next feed/flush/free. _data() returns "" (not NULL) for
 * an empty row so callers don't need to NULL-check. _is_blank() is
 * true when the row is empty or pure whitespace (space / tab). */
const char *term_lite_cur_data(const struct term_lite *t);
size_t term_lite_cur_len(const struct term_lite *t);
int term_lite_cur_is_blank(const struct term_lite *t);

const char *term_lite_bottom_data(const struct term_lite *t);
size_t term_lite_bottom_len(const struct term_lite *t);
int term_lite_bottom_is_blank(const struct term_lite *t);

const char *term_lite_active_data(const struct term_lite *t);
size_t term_lite_active_len(const struct term_lite *t);
int term_lite_active_is_blank(const struct term_lite *t);

#endif /* HAX_TERM_LITE_H */
