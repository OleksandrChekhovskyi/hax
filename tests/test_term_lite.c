/* SPDX-License-Identifier: MIT */
#include "term_lite.h"

#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "util.h"

/* Capture committed rows into a single string. Honors has_newline so
 * the output reads exactly like the original byte stream: rows the
 * producer terminated with \n get a \n appended, a true trailing
 * partial gets none. */
struct capture {
    struct buf out;
    int n_lines;
};

static void on_line(const char *line, size_t len, int has_newline, void *user)
{
    struct capture *c = user;
    buf_append(&c->out, line, len);
    if (has_newline)
        buf_append(&c->out, "\n", 1);
    c->n_lines++;
}

static void cap_init(struct capture *c)
{
    buf_init(&c->out);
    c->n_lines = 0;
}

static void cap_free(struct capture *c)
{
    buf_free(&c->out);
}

static const char *cap_str(struct capture *c)
{
    if (!c->out.data) {
        static const char empty[] = "";
        return empty;
    }
    return c->out.data;
}

/* One-shot helper: feed `in` (length strlen) in a single chunk, then
 * flush, returning the joined output as a newly-allocated string. */
static char *render_oneshot(const char *in)
{
    struct term_lite t;
    term_lite_init(&t);
    struct capture c;
    cap_init(&c);
    term_lite_feed(&t, in, strlen(in), on_line, &c);
    term_lite_flush(&t, on_line, &c);
    term_lite_free(&t);
    char *s = c.out.data ? strdup(cap_str(&c)) : strdup("");
    cap_free(&c);
    return s;
}

static void test_passthrough(void)
{
    char *got = render_oneshot("hello\nworld\n");
    EXPECT_STR_EQ(got, "hello\nworld\n");
    free(got);
}

static void test_unterminated_trailing_partial(void)
{
    /* No \n at end → has_newline=0 on flush. Test's on_line skips the
     * synthesized \n in that case, so output is the literal "hello". */
    char *got = render_oneshot("hello");
    EXPECT_STR_EQ(got, "hello");
    free(got);
}

static void test_cr_overwrite_same_length(void)
{
    /* Classic ninja/meson progress: \r-terminated partials all the
     * same length, only the final state survives. */
    char *got = render_oneshot("[1/5] foo\r[2/5] bar\r[3/5] baz\n");
    EXPECT_STR_EQ(got, "[3/5] baz\n");
    free(got);
}

static void test_cr_overwrite_shorter_leaves_tail(void)
{
    /* Real-terminal behavior: \r without a clear-EOL leaves the tail
     * of the previous content visible. Tools that care emit \r\033[K
     * (the K now reaches term_lite via ctrl_strip's allowlist). */
    char *got = render_oneshot("hello\rwo\n");
    EXPECT_STR_EQ(got, "wollo\n");
    free(got);
}

static void test_cr_with_clear_eol(void)
{
    /* \r + ESC[K + new content → row is fully cleared, then rewritten.
     * Matches what well-behaved progress tools (ninja, cargo) emit. */
    char *got = render_oneshot("hello\r\x1b[Kwo\n");
    EXPECT_STR_EQ(got, "wo\n");
    free(got);
}

static void test_crlf_collapses(void)
{
    char *got = render_oneshot("hello\r\nworld\r\n");
    EXPECT_STR_EQ(got, "hello\nworld\n");
    free(got);
}

static void test_trailing_cr_keeps_line(void)
{
    /* \r at end-of-stream never overwrote anything. Row is still
     * "hello", but it has no trailing \n in the source → has_newline=0. */
    char *got = render_oneshot("hello\r");
    EXPECT_STR_EQ(got, "hello");
    free(got);
}

static void test_backspace_moves_cursor(void)
{
    /* Real \b moves the cursor without erasing. Subsequent writes
     * overwrite. "abc\b\bX" → cursor at col 1 after the two \bs, X
     * overwrites 'b' → "aXc". */
    char *got = render_oneshot("abc\b\bX\n");
    EXPECT_STR_EQ(got, "aXc\n");
    free(got);
}

static void test_backspace_at_empty_is_nop(void)
{
    char *got = render_oneshot("\b\b\bhi\n");
    EXPECT_STR_EQ(got, "hi\n");
    free(got);
}

static void test_backspace_utf8_aware(void)
{
    /* "h\xC3\xA9" places cursor at byte col 3. \b walks back one
     * codepoint to col 1 (start of 'é'). Then 'X' overwrites byte at
     * col 1 — note this leaves the orphan continuation 0xA9 behind!
     * That's the cost of byte-overwrite semantics for codepoint-sized
     * cells. utf8_sanitize downstream replaces orphans with U+FFFD. */
    const char in[] = "h\xC3\xA9\bX\n";
    char *got = render_oneshot(in);
    /* "h" + 'X' (overwrite at col 1) + leftover \xA9 (continuation) + "\n" */
    EXPECT_STR_EQ(got, "hX\xA9\n");
    free(got);
}

static void test_chunk_boundary_at_cr(void)
{
    /* \r in one chunk, follow-up byte in the next: cursor reset
     * survives the boundary. Subsequent X overwrites the first byte. */
    struct term_lite t;
    term_lite_init(&t);
    struct capture c;
    cap_init(&c);
    term_lite_feed(&t, "hello\r", 6, on_line, &c);
    term_lite_feed(&t, "X\n", 2, on_line, &c);
    term_lite_flush(&t, on_line, &c);
    term_lite_free(&t);
    EXPECT_STR_EQ(cap_str(&c), "Xello\n");
    cap_free(&c);
}

static void test_chunk_boundary_byte_by_byte(void)
{
    const char *in = "[1/3] a\r[2/3] bb\r[3/3] ccc\nnext\n";
    struct term_lite t;
    term_lite_init(&t);
    struct capture c;
    cap_init(&c);
    for (size_t i = 0; in[i]; i++)
        term_lite_feed(&t, in + i, 1, on_line, &c);
    term_lite_flush(&t, on_line, &c);
    term_lite_free(&t);
    EXPECT_STR_EQ(cap_str(&c), "[3/3] ccc\nnext\n");
    cap_free(&c);
}

static void test_cur_accessor_reflects_overwrites(void)
{
    /* Live preview reads cursor row directly via the accessor. */
    struct term_lite t;
    term_lite_init(&t);
    term_lite_feed(&t, "[1/5] foo\r[2/5] bar", 19, NULL, NULL);
    EXPECT_MEM_EQ(term_lite_cur_data(&t), term_lite_cur_len(&t), "[2/5] bar", 9);
    EXPECT(!term_lite_cur_is_blank(&t));
    term_lite_free(&t);
}

static void test_cur_is_blank(void)
{
    struct term_lite t;
    term_lite_init(&t);
    EXPECT(term_lite_cur_is_blank(&t));
    term_lite_feed(&t, "  \t ", 4, NULL, NULL);
    EXPECT(term_lite_cur_is_blank(&t));
    term_lite_feed(&t, "x", 1, NULL, NULL);
    EXPECT(!term_lite_cur_is_blank(&t));
    term_lite_free(&t);
}

static void test_blank_lines_preserved(void)
{
    /* \n on an empty row sets has_newline=1 → emitted as a blank line. */
    char *got = render_oneshot("a\n\n\nb\n");
    EXPECT_STR_EQ(got, "a\n\n\nb\n");
    free(got);
}

/* ---------- CSI A (CUU) — cursor up ---------- */

static void test_cuu_overwrites_previous_row(void)
{
    /* The vitest pattern in miniature: write rows, CUU back to one
     * of them, overwrite. The buffered row is updated in place; the
     * model only sees the final state when flush drains the ring. */
    char *got = render_oneshot("row1\nrow2\n\x1b[1Anewrow2\n");
    EXPECT_STR_EQ(got, "row1\nnewrow2\n");
    free(got);
}

static void test_cuu_clamps_at_top(void)
{
    /* CUU N where N exceeds rows-above-cursor clamps at row 0. */
    char *got = render_oneshot("abc\n\x1b[5Axyz\n");
    /* After "abc\n": cursor at row 1 col 0. CUU 5 → row 0 col 0.
     * "xyz" overwrites 'a','b','c' → row 0 = "xyz". \n advances. */
    EXPECT_STR_EQ(got, "xyz\n");
    free(got);
}

static void test_cuu_default_param_is_one(void)
{
    /* ESC[A with no param means CUU 1. */
    char *got = render_oneshot("row1\nrow2\n\x1b[Aoverwrite\n");
    EXPECT_STR_EQ(got, "row1\noverwrite\n");
    free(got);
}

/* ---------- CSI B (CUD) — cursor down ---------- */

static void test_cud_advances_cursor(void)
{
    /* CUD past row_count creates new empty rows. Their has_newline
     * stays 0, so flush skips them (empty + unterminated). */
    char *got = render_oneshot("row1\n\x1b[2Bend\n");
    /* row 0 = "row1" h=1, then CUD 2 from row 1 → row 3, both row 1
     * and row 2 are created empty with h=0. Write "end" at row 3,
     * then \n marks h=1 and advances. flush emits row 0, skips
     * empty row 1 + row 2 (h=0), emits row 3 "end\n". */
    EXPECT_STR_EQ(got, "row1\nend\n");
    free(got);
}

/* ---------- CSI G (CHA) — cursor horizontal absolute ---------- */

static void test_cha_resets_cursor_then_el_clears(void)
{
    /* The biome / ora / indicatif spinner-cleanup pattern:
     * "<glyph> ESC[1G ESC[K" — write a frame, jump cursor to col 1,
     * erase to EOL. Without CHA, EL would fire from the byte position
     * past the glyph and miss it entirely. */
    char *got = render_oneshot("\xE2\xA0\x99\x1b[1G\x1b[K");
    /* Glyph fully cleared; trailing partial empty → flush emits nothing. */
    EXPECT_STR_EQ(got, "");
    free(got);
}

static void test_cha_default_param_is_one(void)
{
    char *got = render_oneshot("hello\x1b[GX\n");
    /* CHA with no param defaults to 1 (col 1, our col 0). 'X'
     * overwrites 'h' → "Xello". */
    EXPECT_STR_EQ(got, "Xello\n");
    free(got);
}

/* ---------- CSI C/D (CUF/CUB) — column motion ---------- */

static void test_cuf_advances_col_with_padding(void)
{
    /* CUF past row content lands cursor in "blank" space; the next
     * write pads with spaces to keep the row buffer well-formed. */
    char *got = render_oneshot("ab\x1b[3CX\n");
    /* "ab" col=2, CUF 3 → col=5, write 'X' at col 5 → "ab" + 3 spaces + 'X'. */
    EXPECT_STR_EQ(got, "ab   X\n");
    free(got);
}

static void test_cub_clamps_at_col_zero(void)
{
    char *got = render_oneshot("hello\x1b[10DX\n");
    /* CUB 10 from col 5 clamps to col 0; 'X' overwrites 'h'. */
    EXPECT_STR_EQ(got, "Xello\n");
    free(got);
}

/* ---------- CSI E/F (CNL/CPL) — line motion with col reset ---------- */

static void test_cnl_advances_and_resets_col(void)
{
    char *got = render_oneshot("abc\x1b[ENEW\n");
    /* "abc" col=3, CNL 1 → row 1 col 0. "NEW" → row 1 = "NEW". */
    EXPECT_STR_EQ(got, "abc\nNEW\n");
    free(got);
}

static void test_cpl_retreats_and_resets_col(void)
{
    char *got = render_oneshot("oldA\nlongerB\x1b[FX\n");
    /* CPL 1 from row 1 col 7 → row 0 col 0. 'X' overwrites 'o' →
     * "Xld" + leftover... wait, "oldA" is 4 bytes, X at col 0 keeps
     * "Xld" + 'A' = "XldA". \n advances. */
    EXPECT_STR_EQ(got, "XldA\nlongerB\n");
    free(got);
}

/* ---------- CSI d (VPA) — vertical position absolute ---------- */

static void test_vpa_jumps_to_row(void)
{
    char *got = render_oneshot("a\nb\nc\n\x1b[2dX\n");
    /* "a\nb\nc\n" creates rows 0,1,2 with content + row 3 cursor empty.
     * VPA 2 → row 1 (1-indexed → 0-indexed). 'X' overwrites 'b' at
     * col 0; row 1 = "X". \n marks row 1 (was already h=1, stays). */
    EXPECT_STR_EQ(got, "a\nX\nc\n");
    free(got);
}

/* ---------- CSI H/f (CUP) — absolute row;col ---------- */

static void test_cup_sets_both_axes(void)
{
    char *got = render_oneshot("\nrow1\nrow2\nrow3\n\x1b[2;3HX\n");
    /* Set up 3 content rows + a leading blank row + trailing cursor.
     * CUP 2;3 → row 1 (1-indexed) col 2. row 1 was "row1"; X overwrites
     * byte at col 2 ('w' → 'X') → "roX1". */
    EXPECT_STR_EQ(got, "\nroX1\nrow2\nrow3\n");
    free(got);
}

static void test_cup_no_args_goes_home(void)
{
    /* ESC[H with no params is "cursor home" (row 1 col 1 = row 0 col 0). */
    char *got = render_oneshot("abc\nfoo\x1b[HX\n");
    /* After "abc\nfoo": cursor row 1 col 3. ESC[H → row 0 col 0.
     * 'X' overwrites 'a'. \n advances row 0 → row 1, col 0. */
    EXPECT_STR_EQ(got, "Xbc\nfoo\n");
    free(got);
}

/* ---------- CSI s/u (SCOSC/SCORC) — save/restore cursor ---------- */

static void test_scosc_scorc_round_trip(void)
{
    /* The classic spinner/marker pattern: write a marker, save, do
     * other stuff, restore, overwrite the marker. */
    char *got = render_oneshot("foo\x1b[s bar\x1b[uX\n");
    /* "foo" col=3, save (row=0, col=3). " bar" → row="foo bar" col=7.
     * Restore → col=3. 'X' overwrites ' ' at col 3 → "fooXbar". */
    EXPECT_STR_EQ(got, "fooXbar\n");
    free(got);
}

static void test_scorc_without_scosc_goes_home(void)
{
    char *got = render_oneshot("abc\x1b[uX\n");
    /* No prior save → restore goes to (0, 0). 'X' overwrites 'a'. */
    EXPECT_STR_EQ(got, "Xbc\n");
    free(got);
}

/* ---------- CSI K (EL) — erase in line ---------- */

static void test_el_truncates_to_eol(void)
{
    /* ESC[K with cursor in middle of row truncates from cursor onward. */
    char *got = render_oneshot("abcdef\rxyz\x1b[K\n");
    /* "abcdef" col=6. \r col=0. "xyz" overwrites 'a','b','c' → row =
     * "xyzdef" col=3. \K truncates from col 3 → "xyz". \n advances. */
    EXPECT_STR_EQ(got, "xyz\n");
    free(got);
}

static void test_el_2_clears_whole_row(void)
{
    /* ESC[2K wipes the whole row regardless of cursor position. */
    char *got = render_oneshot("garbage\x1b[2K\rclean\n");
    EXPECT_STR_EQ(got, "clean\n");
    free(got);
}

/* ---------- CSI J (ED) — erase in display ---------- */

static void test_ed_clears_below(void)
{
    /* ESC[J from a row with content below clears those rows. */
    char *got = render_oneshot("a\nb\nc\n\x1b[2A\x1b[Jx\n");
    /* "a\nb\nc\n" → 4 rows, cursor at row 3 col 0. CUU 2 → row 1.
     * \x1b[J: truncate row 1 from col 0 (was "b" → empty), drop rows
     * 2-3 (count was 4, now 2). Write "x" at row 1 col 0. \n marks. */
    EXPECT_STR_EQ(got, "a\nx\n");
    free(got);
}

/* ---------- multi-row redraw smoke test ---------- */

static void test_vitest_window_redraw(void)
{
    /* Caricature of vitest's WindowRenderer: 3-row window, redraw
     * with cursor-up + clear-line, then a final summary. Each redraw
     * sequence does \r before \K so cursor lands at col 0 — without
     * that, a real terminal would also produce ragged output (the
     * cursor stays at the previous col after CUU, and writes there).
     * The model should see only the final state of the window. */
    const char *in =
        /* initial 3-row window — last row has no trailing \n */
        "row A 1/3\nrow B 1/3\nrow C 1/3"
        /* climb to row 0, clearing each row at col 0 along the way */
        "\r\x1b[K"
        "\x1b[1A\r\x1b[K"
        "\x1b[1A\r\x1b[K"
        /* redraw with second-iteration content */
        "row A 2/3\nrow B 2/3\nrow C 2/3"
        /* clear again */
        "\r\x1b[K"
        "\x1b[1A\r\x1b[K"
        "\x1b[1A\r\x1b[K"
        /* final summary */
        "DONE A\nDONE B\nDONE C\n";
    char *got = render_oneshot(in);
    EXPECT_STR_EQ(got, "DONE A\nDONE B\nDONE C\n");
    free(got);
}

/* ---------- settle policy: \n-driven, no chunk dependency ---------- */

static void test_settle_commits_each_completed_line(void)
{
    /* Pure-linear stream: each \n graduates its row immediately, in
     * the same feed call. No chunk-boundary delay — that's the
     * property that fixes the "preamble line stuck in buffer until
     * the next chunk arrives" UX issue when producers have
     * unpredictable inter-chunk timing (npm output is a textbook
     * case). */
    struct term_lite t;
    term_lite_init(&t);
    struct capture c;
    cap_init(&c);
    term_lite_feed(&t, "a\n", 2, on_line, &c);
    EXPECT(c.n_lines == 1); /* "a" settles right away */
    EXPECT_STR_EQ(cap_str(&c), "a\n");
    term_lite_feed(&t, "b\n", 2, on_line, &c);
    EXPECT(c.n_lines == 2);
    term_lite_flush(&t, on_line, &c);
    term_lite_free(&t);
    EXPECT_STR_EQ(cap_str(&c), "a\nb\n");
    cap_free(&c);
}

static void test_settle_holds_unterminated_line(void)
{
    /* A line without a trailing \n stays in the buffer — cur_row sits
     * on it, settle skips. Only flush emits it (with has_newline=0
     * so consumers can reconstruct the original byte stream). */
    struct term_lite t;
    term_lite_init(&t);
    struct capture c;
    cap_init(&c);
    term_lite_feed(&t, "partial", 7, on_line, &c);
    EXPECT(c.n_lines == 0);
    term_lite_flush(&t, on_line, &c);
    term_lite_free(&t);
    /* Flush emits "partial" with has_newline=0 — the test helper
     * appends \n only when has_newline=1, so output is just
     * "partial". */
    EXPECT_STR_EQ(cap_str(&c), "partial");
    cap_free(&c);
}

static void test_settle_doesnt_emit_in_progress_rewrite(void)
{
    /* ninja/cargo-style \r progress: row 0 keeps getting rewritten,
     * cursor stays at row 0 (no \n until the final). Settle never
     * fires for row 0 (cur_row==0). Only the final \n graduates. */
    struct term_lite t;
    term_lite_init(&t);
    struct capture c;
    cap_init(&c);
    term_lite_feed(&t, "[1/3] a\r", 8, on_line, &c);
    EXPECT(c.n_lines == 0);
    term_lite_feed(&t, "[2/3] b\r", 8, on_line, &c);
    EXPECT(c.n_lines == 0);
    term_lite_feed(&t, "[3/3] c\n", 8, on_line, &c);
    /* The final \n advances cur_row to 1; settle drops row 0. */
    EXPECT(c.n_lines == 1);
    EXPECT_STR_EQ(cap_str(&c), "[3/3] c\n");
    term_lite_flush(&t, on_line, &c);
    term_lite_free(&t);
    EXPECT_STR_EQ(cap_str(&c), "[3/3] c\n");
    cap_free(&c);
}

static void test_margin_settle_commits_preamble(void)
{
    /* Playwright-style stream: preamble lines, then a CUU+write+\n
     * cycle that repaints one row over and over. The "running N
     * tests" marker is followed by \n\n so the [1A lands on an
     * empty row, not on the marker — the marker must commit during
     * streaming (margin = max CUU depth = 1 → rows older than
     * cur_row - 1 settle), even though cursor_moved_up has latched.
     * The active rewrite row stays buffered until flush. */
    struct term_lite t;
    term_lite_init(&t);
    struct capture c;
    cap_init(&c);
    /* Preamble + the \n\n separator that playwright emits before
     * starting the test-progress repaint cycle. */
    term_lite_feed(&t, "preamble\nrunning 90 tests\n\n", 27, on_line, &c);
    /* First [1A+\K+content+\n cycle: cursor up 1 lands on the empty
     * row above the trailing empty, repaint, advance back. */
    term_lite_feed(&t, "\x1b[1A\x1b[2K[1/90] first test\n", 26, on_line, &c);
    /* Second cycle. */
    term_lite_feed(&t, "\x1b[1A\x1b[2K[2/90] second test\n", 27, on_line, &c);
    /* Preamble + "running 90 tests" must have settled. The active
     * progress row (the one being repainted) hasn't. */
    EXPECT(strstr(cap_str(&c), "preamble\n") != NULL);
    EXPECT(strstr(cap_str(&c), "running 90 tests\n") != NULL);
    EXPECT(strstr(cap_str(&c), "[1/90]") == NULL); /* not yet settled */
    term_lite_flush(&t, on_line, &c);
    term_lite_free(&t);
    /* Flush emits the final state of the active row. */
    EXPECT(strstr(cap_str(&c), "[2/90] second test") != NULL);
    EXPECT(strstr(cap_str(&c), "[1/90]") == NULL); /* overwritten in place */
    cap_free(&c);
}

static void test_active_row_returns_in_progress_content(void)
{
    /* The active accessor should return the row that actually has
     * content — what the spinner label wants to show. For an
     * unterminated in-progress line (no \n yet), that's cur_row. */
    struct term_lite t;
    term_lite_init(&t);
    term_lite_feed(&t, "running...", 10, NULL, NULL);
    /* No \n yet, row stays in buffer. cur and active both point at it. */
    EXPECT(!term_lite_cur_is_blank(&t));
    EXPECT(!term_lite_active_is_blank(&t));
    EXPECT_MEM_EQ(term_lite_active_data(&t), term_lite_active_len(&t), "running...", 10);
    term_lite_free(&t);
}

static void test_windowed_growing_window_is_handled(void)
{
    /* Vitest's failure mode: the WindowRenderer's clearWindow uses the
     * *previous* render's windowHeight for [1A's, then writes the
     * *current* (sometimes larger) window. When the window grows,
     * cur_row goes UP by old_height and DOWN by new_height — net
     * forward motion past the old high-water. The settle margin must
     * cover the full new window depth (max - min, including the
     * forward extension), not just the [1A count, otherwise the
     * top rows of the new window settle prematurely as duplicates. */
    struct term_lite t;
    term_lite_init(&t);
    struct capture c;
    cap_init(&c);
    /* Initial 4-row window. */
    term_lite_feed(&t, "row1\nrow2\nrow3\nrow4\n", 20, on_line, &c);
    /* clearWindow with windowHeight=4 (3× [1A), then write 6-row
     * window (5 \n's) — window grew by 2 rows. */
    const char *grow = "\x1b[3A"
                       "new1\nnew2\nnew3\nnew4\nnew5\nnew6\n";
    term_lite_feed(&t, grow, strlen(grow), on_line, &c);
    /* Another redraw cycle on the new larger window: 5× [1A, then
     * 6-row content. If max_cuu_distance was set correctly to 8 (the
     * full new-window depth), all 6 new rows stay buffered. */
    const char *cycle = "\x1b[5A"
                        "v1\nv2\nv3\nv4\nv5\nv6\n";
    term_lite_feed(&t, cycle, strlen(cycle), on_line, &c);
    term_lite_flush(&t, on_line, &c);
    term_lite_free(&t);
    /* Final state should contain only v1..v6, no stale "new" rows. */
    EXPECT(strstr(cap_str(&c), "v6") != NULL);
    /* Stale frames from the second cycle's writes shouldn't appear
     * (they're overwritten in place by the third cycle). */
    EXPECT(strstr(cap_str(&c), "new1\nnew2") == NULL);
    cap_free(&c);
}

static void test_per_chunk_disabled_after_cuu(void)
{
    /* Once the producer does CUU, it's signaling windowed redraws.
     * Per-chunk settle must shut off — chunks may split mid-cycle and
     * leave a window row "untouched in current chunk" right before it
     * gets edited again. Settling such a row would emit a stale frame
     * and let the next redraw produce a duplicate alongside.
     *
     * Stream pattern: render once (no CUU yet — first frame), then
     * a clearWindow + redraw cycle. PTY-realistic chunking: split
     * the stream into pieces of ~50 bytes (smaller than one redraw
     * cycle so chunk boundaries fall mid-cycle). Without the latch,
     * each split-chunk would settle 1/3 frames as the cursor walked
     * through; with it, only flush emits, and emit is the final 2/3
     * state. */
    const char *in = "row A 1/3\nrow B 1/3\nrow C 1/3"
                     "\r\x1b[K"
                     "\x1b[1A\r\x1b[K"
                     "\x1b[1A\r\x1b[K"
                     "row A 2/3\nrow B 2/3\nrow C 2/3";
    struct term_lite t;
    term_lite_init(&t);
    struct capture c;
    cap_init(&c);
    size_t chunk = 50;
    size_t in_len = strlen(in);
    for (size_t i = 0; i < in_len; i += chunk) {
        size_t sz = i + chunk > in_len ? in_len - i : chunk;
        term_lite_feed(&t, in + i, sz, on_line, &c);
    }
    /* Some pre-CUU per-chunk settles may fire on the first render's
     * rows (the producer hasn't hinted at windowing yet), but the
     * window rows under active redraw must NOT leak as duplicates. */
    int pre_flush_lines = c.n_lines;
    term_lite_flush(&t, on_line, &c);
    term_lite_free(&t);
    /* The output must not contain "1/3" duplicates. The final state
     * is the 2/3 frame. Allow a few legit emissions of the first
     * render before CUU latches; total lines stay tight. */
    EXPECT(c.n_lines <= 6); /* bounded — not 100s of duplicates */
    EXPECT(strstr(cap_str(&c), "row A 2/3") != NULL);
    EXPECT(strstr(cap_str(&c), "row B 2/3") != NULL);
    EXPECT(strstr(cap_str(&c), "row C 2/3") != NULL);
    (void)pre_flush_lines;
    cap_free(&c);
}

static void test_settle_within_single_chunk(void)
{
    /* A single chunk with multiple \n's settles each completed row
     * in the same feed call. Pure-linear streams don't accumulate in
     * the buffer waiting for a chunk boundary. */
    struct term_lite t;
    term_lite_init(&t);
    struct capture c;
    cap_init(&c);
    term_lite_feed(&t, "a\nb\nc\nd\n", 8, on_line, &c);
    EXPECT(c.n_lines == 4);
    EXPECT_STR_EQ(cap_str(&c), "a\nb\nc\nd\n");
    term_lite_flush(&t, on_line, &c);
    term_lite_free(&t);
    EXPECT_STR_EQ(cap_str(&c), "a\nb\nc\nd\n");
    cap_free(&c);
}

static void test_ring_overflow_evicts(void)
{
    /* Even though pure-linear settle keeps cur_row at 1 after each
     * \n in a streaming scenario, a single huge chunk fills the ring
     * before the end-of-feed settle runs — eviction is what bounds
     * memory mid-feed. Tested as a safety net. */
    char *big = xmalloc(16 * (TERM_LITE_RING_CAP + 5));
    size_t pos = 0;
    int total = TERM_LITE_RING_CAP + 5;
    for (int i = 0; i < total; i++)
        pos += (size_t)snprintf(big + pos, 16, "row %d\n", i);

    struct term_lite t;
    term_lite_init(&t);
    struct capture c;
    cap_init(&c);
    term_lite_feed(&t, big, pos, on_line, &c);
    term_lite_flush(&t, on_line, &c);
    term_lite_free(&t);
    /* All rows committed in order — either via ring eviction during
     * feed or via the end-of-feed settle drop. */
    EXPECT(c.n_lines == total);
    free(big);
    cap_free(&c);
}

/* ---------- bottom accessors ---------- */

static void test_bottom_tracks_last_row(void)
{
    /* For pure-linear streams bottom == cursor. */
    struct term_lite t;
    term_lite_init(&t);
    term_lite_feed(&t, "row1\nrow2", 9, NULL, NULL);
    EXPECT_MEM_EQ(term_lite_bottom_data(&t), term_lite_bottom_len(&t), "row2", 4);
    EXPECT_MEM_EQ(term_lite_cur_data(&t), term_lite_cur_len(&t), "row2", 4);
    term_lite_free(&t);
}

static void test_bottom_stable_under_cuu(void)
{
    /* After CUU, cursor is somewhere above; bottom keeps showing the
     * highest-row content. Aggressive settle drops rows older than
     * cur_row - max_cuu_distance, so we need a stream that establishes
     * a large enough window depth to keep all 3 rows buffered. */
    struct term_lite t;
    term_lite_init(&t);
    /* 3 rows, then CUU 2 — establishes max_cuu_distance=2 so all 3
     * rows stay buffered for the rest of the test. */
    term_lite_feed(&t, "top\nmid\nbot\n\x1b[2A", 16, NULL, NULL);
    /* Cursor is now at "mid" (row 1). Bottom is the empty trailing
     * row left by the third \n. The active accessor walks back to
     * "bot" — which is what tool_render's spinner uses. */
    EXPECT_MEM_EQ(term_lite_active_data(&t), term_lite_active_len(&t), "bot", 3);
    /* Another CUU brings cursor to "top". Active still shows "bot". */
    term_lite_feed(&t, "\x1b[1A", 4, NULL, NULL);
    EXPECT_MEM_EQ(term_lite_cur_data(&t), term_lite_cur_len(&t), "top", 3);
    EXPECT_MEM_EQ(term_lite_active_data(&t), term_lite_active_len(&t), "bot", 3);
    term_lite_free(&t);
}

int main(void)
{
    test_passthrough();
    test_unterminated_trailing_partial();
    test_cr_overwrite_same_length();
    test_cr_overwrite_shorter_leaves_tail();
    test_cr_with_clear_eol();
    test_crlf_collapses();
    test_trailing_cr_keeps_line();
    test_backspace_moves_cursor();
    test_backspace_at_empty_is_nop();
    test_backspace_utf8_aware();
    test_chunk_boundary_at_cr();
    test_chunk_boundary_byte_by_byte();
    test_cur_accessor_reflects_overwrites();
    test_cur_is_blank();
    test_blank_lines_preserved();
    test_cuu_overwrites_previous_row();
    test_cuu_clamps_at_top();
    test_cuu_default_param_is_one();
    test_cud_advances_cursor();
    test_cha_resets_cursor_then_el_clears();
    test_cha_default_param_is_one();
    test_cuf_advances_col_with_padding();
    test_cub_clamps_at_col_zero();
    test_cnl_advances_and_resets_col();
    test_cpl_retreats_and_resets_col();
    test_vpa_jumps_to_row();
    test_cup_sets_both_axes();
    test_cup_no_args_goes_home();
    test_scosc_scorc_round_trip();
    test_scorc_without_scosc_goes_home();
    test_el_truncates_to_eol();
    test_el_2_clears_whole_row();
    test_ed_clears_below();
    test_vitest_window_redraw();
    test_settle_commits_each_completed_line();
    test_settle_holds_unterminated_line();
    test_settle_doesnt_emit_in_progress_rewrite();
    test_settle_within_single_chunk();
    test_margin_settle_commits_preamble();
    test_active_row_returns_in_progress_content();
    test_windowed_growing_window_is_handled();
    test_per_chunk_disabled_after_cuu();
    test_ring_overflow_evicts();
    test_bottom_tracks_last_row();
    test_bottom_stable_under_cuu();
    T_REPORT();
}
