/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "util.h"
#include "terminal/input.h"
#include "terminal/input_core.h"

/* ---------- helpers ---------- */

static struct input *new_with(const char *initial)
{
    struct input *in = input_new();
    if (initial)
        input_core_buf_set(in, initial);
    return in;
}

/* ---------- compute_layout ---------- */

static void test_layout_empty(void)
{
    struct input_layout L;
    input_core_compute_layout("", 0, 0, 2, 80, &L);
    EXPECT(L.cursor_row == 0);
    EXPECT(L.cursor_col == 2);
    EXPECT(L.end_row == 0);
    EXPECT(L.end_col == 2);
    EXPECT(L.total_rows == 1);
}

static void test_layout_single_line_cursor_positions(void)
{
    /* "hello" with prompt_w=2, cursor at 0/3/5 */
    struct input_layout L;
    input_core_compute_layout("hello", 5, 0, 2, 80, &L);
    EXPECT(L.cursor_row == 0);
    EXPECT(L.cursor_col == 2);
    EXPECT(L.end_col == 7);
    EXPECT(L.total_rows == 1);

    input_core_compute_layout("hello", 5, 3, 2, 80, &L);
    EXPECT(L.cursor_col == 5);

    input_core_compute_layout("hello", 5, 5, 2, 80, &L);
    EXPECT(L.cursor_col == 7);
}

static void test_layout_multi_line(void)
{
    /* "ab\ncd" — cursor at 0, 2 (on the \n), 3 (start of line 2), 5 (end). */
    struct input_layout L;
    const char *s = "ab\ncd";

    input_core_compute_layout(s, 5, 0, 2, 80, &L);
    EXPECT(L.cursor_row == 0 && L.cursor_col == 2);

    /* cursor on the '\n' itself — still on row 0, after "ab" */
    input_core_compute_layout(s, 5, 2, 2, 80, &L);
    EXPECT(L.cursor_row == 0 && L.cursor_col == 4);

    /* cursor right after the '\n' — start of line 2, indented to prompt_w */
    input_core_compute_layout(s, 5, 3, 2, 80, &L);
    EXPECT(L.cursor_row == 1 && L.cursor_col == 2);

    /* cursor at end of "cd" */
    input_core_compute_layout(s, 5, 5, 2, 80, &L);
    EXPECT(L.cursor_row == 1 && L.cursor_col == 4);
    EXPECT(L.total_rows == 2);
}

static void test_layout_buffer_ending_in_newline(void)
{
    /* "ab\n" with cursor at end — on a fresh empty row indented to prompt_w. */
    struct input_layout L;
    input_core_compute_layout("ab\n", 3, 3, 2, 80, &L);
    EXPECT(L.cursor_row == 1);
    EXPECT(L.cursor_col == 2);
    EXPECT(L.total_rows == 2);
}

static void test_layout_continuation_indent(void)
{
    /* Three logical lines; each continuation starts at col=prompt_w. */
    struct input_layout L;
    input_core_compute_layout("a\nb\nc", 5, 5, 2, 80, &L);
    EXPECT(L.cursor_row == 2);
    EXPECT(L.cursor_col == 3); /* "  c" → col 2 + 1 */
    EXPECT(L.total_rows == 3);
}

static void test_layout_bidi_substituted(void)
{
    /* U+202E (right-to-left override, "Trojan Source"). wcwidth on
     * mac/glibc may return 0 or 1; we explicitly substitute and treat
     * as 1 col so display can't be reordered by attacker-controlled
     * paste in the persistent scrollback. UTF-8 encoding: E2 80 AE. */
    struct input_layout L;
    input_core_compute_layout("\xe2\x80\xae", 3, 3, 0, 80, &L);
    EXPECT(L.end_col == 1);

    /* U+061C (Arabic Letter Mark) — another bidi control that
     * commonly slips through wcwidth filters. UTF-8: D8 9C. */
    input_core_compute_layout("\xd8\x9c", 2, 2, 0, 80, &L);
    EXPECT(L.end_col == 1);

    /* U+2060 (word joiner) — invisible, in the U+2060–U+206F range.
     * UTF-8: E2 81 A0. */
    input_core_compute_layout("\xe2\x81\xa0", 3, 3, 0, 80, &L);
    EXPECT(L.end_col == 1);
}

static void test_layout_c1_in_utf8(void)
{
    /* U+009B (CSI in C1) encoded as UTF-8 (0xC2 0x9B). wcwidth
     * returns -1 so layout treats it as 1 col (matches the '?'
     * substitute that emit_safe_span will write to the terminal,
     * preventing C1 escape interpretation). */
    struct input_layout L;
    input_core_compute_layout("\xc2\x9b", 2, 2, 0, 80, &L);
    EXPECT(L.end_col == 1);
    EXPECT(L.cursor_col == 1);
    EXPECT(L.total_rows == 1);
}

static void test_layout_combining_marks(void)
{
    /* "e" + U+0301 (combining acute, 2-byte UTF-8 0xCC 0x81). The
     * combining mark has width 0, so the cursor at end sits at col 1
     * (just past the 'e'), not col 2. */
    struct input_layout L;
    input_core_compute_layout("e\xcc\x81", 3, 3, 0, 80, &L);
    EXPECT(L.end_col == 1);
    EXPECT(L.cursor_col == 1);
    EXPECT(L.total_rows == 1);
}

static void test_layout_tabs(void)
{
    /* Each tab is exactly TAB_WIDTH columns. With prompt_w=2:
     *   col 2 + tab → 6, then "x" → 7. */
    struct input_layout L;
    input_core_compute_layout("\tx", 2, 2, 2, 80, &L);
    EXPECT(L.cursor_row == 0);
    EXPECT(L.cursor_col == 7);
    EXPECT(L.end_col == 7);

    /* With cursor before the tab, col stays at prompt_w. */
    input_core_compute_layout("\tx", 2, 0, 2, 80, &L);
    EXPECT(L.cursor_col == 2);

    /* Two tabs from col 2 advance by TAB_WIDTH each: 2 → 6 → 10. */
    input_core_compute_layout("\t\t", 2, 2, 2, 80, &L);
    EXPECT(L.end_col == 10);

    /* A tab wider than the entire row can't fit anywhere — the walker
     * emits it at the row's starting column (cont_indent_col) and lets
     * it overflow visually. Breaking to a fresh row first would just
     * re-overflow with no progress, so the walker holds it in place. */
    input_core_compute_layout("\t", 1, 1, 0, 2, &L);
    EXPECT(L.end_row == 0);
    EXPECT(L.end_col == 4);
}

static void test_layout_soft_wrap_char(void)
{
    /* prompt_w=2, cols=10, "0123456789" (10 chars, no spaces). With
     * no word boundary, the walker falls back to char-wrap. The wrap
     * threshold reserves the terminal's last cell (post-emit col must
     * stay < cols) to avoid the phantom-cursor state, so row 0 fits
     * cols 2..8 (7 chars). The remaining 3 chars land on row 1
     * indented to cont_indent (= prompt_w), so end_col = 2 + 3 = 5. */
    struct input_layout L;
    input_core_compute_layout("0123456789", 10, 10, 2, 10, &L);
    EXPECT(L.total_rows == 2);
    EXPECT(L.end_row == 1);
    EXPECT(L.end_col == 5);
}

static void test_layout_soft_wrap_word(void)
{
    /* prompt_w=2, cols=10, "hello world" (11 chars with a space at
     * index 5). The trailing word "world" doesn't fit at col 8 (would
     * end at col 13 > cols=10), so it moves to row 1 indented to
     * cont_indent (2). end_col after "world" on row 1 = 2 + 5 = 7. */
    struct input_layout L;
    input_core_compute_layout("hello world", 11, 11, 2, 10, &L);
    EXPECT(L.total_rows == 2);
    EXPECT(L.end_row == 1);
    EXPECT(L.end_col == 7);
}

static void test_layout_soft_wrap_drops_boundary_space(void)
{
    /* "abcdefg ij" in cols=10 prompt_w=2: "abcdefg" (7 chars) fills
     * row 0 cols 2..8, leaving the reserved last cell empty. The
     * following space would land at col 9 (== cols-1; the reserved
     * cell) — it's the wrap point, so the walker drops it and breaks.
     * Row 1 starts cleanly with "ij" at cont_indent (no leading
     * space). */
    struct input_layout L;
    input_core_compute_layout("abcdefg ij", 10, 10, 2, 10, &L);
    EXPECT(L.total_rows == 2);
    EXPECT(L.end_row == 1);
    EXPECT(L.end_col == 4); /* 2 + len("ij") */
}

static void test_layout_soft_wrap_cursor_after_wrap(void)
{
    /* Cursor in the wrapped word lands at its new row/col, not the
     * pre-wrap position it would have occupied if it stayed on row 0.
     * cursor=6 is on the 'w' of "world"; after wrap "world" lives at
     * row 1 cont_indent (= 2). */
    struct input_layout L;
    input_core_compute_layout("hello world", 11, 6, 2, 10, &L);
    EXPECT(L.cursor_row == 1);
    EXPECT(L.cursor_col == 2);

    /* cursor=8 is the 'r' (third char of "world") — two cells in,
     * so col = cont_indent + 2 = 4. */
    input_core_compute_layout("hello world", 11, 8, 2, 10, &L);
    EXPECT(L.cursor_row == 1);
    EXPECT(L.cursor_col == 4);
}

/* ---------- render walker callback ---------- */

/* Recorder: appends a transcript of walker events so tests can assert
 * the exact glyph + row-break stream. '/' is the row-break sigil; raw
 * glyph bytes (incl. the TAB → 4-space and unsafe-byte → '?'
 * substitutions the walker performs) pass through verbatim. */
struct rec {
    char buf[256];
    size_t n;
};

static void rec_emit(const struct input_render_event *ev, void *user)
{
    struct rec *r = user;
    if (ev->kind == INPUT_RENDER_ROW_BREAK) {
        if (r->n < sizeof(r->buf) - 1)
            r->buf[r->n++] = '/';
        return;
    }
    for (size_t i = 0; i < ev->n && r->n < sizeof(r->buf) - 1; i++)
        r->buf[r->n++] = ev->bytes[i];
}

static void test_render_word_wrap_emits_break(void)
{
    /* "hello world" in cols=10 prompt=2: row 0 holds "hello " (the
     * trailing space lands at col 7), row 1 holds "world" replayed at
     * cont_indent. The walker emits one ROW_BREAK between them. */
    struct rec r = {0};
    input_core_render("hello world", 11, 0, 2, 2, 10, rec_emit, &r, NULL);
    r.buf[r.n] = '\0';
    EXPECT_STR_EQ(r.buf, "hello /world");
}

static void test_render_drops_boundary_space(void)
{
    /* Space exactly at the row boundary: dropped, no leading space
     * on the next row. The reserved last cell (cols-1) means
     * "abcdefg" fills row 0 cols 2..8 and the space at col 9 is the
     * wrap point. */
    struct rec r = {0};
    input_core_render("abcdefg ij", 10, 0, 2, 2, 10, rec_emit, &r, NULL);
    r.buf[r.n] = '\0';
    EXPECT_STR_EQ(r.buf, "abcdefg/ij");
}

static void test_render_char_wrap_fallback(void)
{
    /* No space — word longer than the row. With the reserved last
     * cell, row 0 fits 7 cells (cols 2-8); break; "hijk" on row 1. */
    struct rec r = {0};
    input_core_render("abcdefghijk", 11, 0, 2, 2, 10, rec_emit, &r, NULL);
    r.buf[r.n] = '\0';
    EXPECT_STR_EQ(r.buf, "abcdefg/hijk");
}

static void test_render_overlong_word_after_wrap_char_wraps(void)
{
    /* Once a word has moved to a fresh row, further overflow in that
     * same overlong token must char-wrap. It must not replay the
     * token tail again and leave a one-character orphan row. */
    struct rec r = {0};
    input_core_render("hello abcdefghijk", 17, 0, 2, 2, 10, rec_emit, &r, NULL);
    r.buf[r.n] = '\0';
    EXPECT_STR_EQ(r.buf, "hello /abcdefg/hijk");
}

static void test_render_hard_newline_indents(void)
{
    /* A literal '\n' breaks to cont_indent and is not itself emitted
     * as a glyph (the row break carries the semantics). */
    struct rec r = {0};
    input_core_render("a\nb", 3, 0, 2, 2, 80, rec_emit, &r, NULL);
    r.buf[r.n] = '\0';
    EXPECT_STR_EQ(r.buf, "a/b");
}

static void test_render_tab_expands(void)
{
    /* Tab decodes as INPUT_CORE_TAB_WIDTH (=4) spaces. */
    struct rec r = {0};
    input_core_render("a\tb", 3, 0, 0, 0, 80, rec_emit, &r, NULL);
    r.buf[r.n] = '\0';
    EXPECT_STR_EQ(r.buf, "a    b");
}

static void test_render_unsafe_substituted(void)
{
    /* C0 control bytes (here 0x07 / BEL) substitute to '?'. */
    struct rec r = {0};
    input_core_render("a\x07"
                      "b",
                      3, 0, 0, 0, 80, rec_emit, &r, NULL);
    r.buf[r.n] = '\0';
    EXPECT_STR_EQ(r.buf, "a?b");
}

/* Counter recorder: tracks how many 'a' glyphs were emitted before
 * the first combining-mark glyph fired. Used to verify that a
 * combining mark arriving while the word-buffer is at capacity is
 * flushed AFTER the buffered word, not before. */
struct order_rec {
    int a_count;
    int combining_after_a_count;
    int combining_seen;
};

static void order_cb(const struct input_render_event *ev, void *user)
{
    struct order_rec *o = user;
    if (ev->kind != INPUT_RENDER_GLYPH)
        return;
    if (ev->n == 1 && ev->bytes[0] == 'a') {
        o->a_count++;
    } else if (!o->combining_seen && ev->width == 0) {
        o->combining_after_a_count = o->a_count;
        o->combining_seen = 1;
    }
}

static void test_render_combining_after_full_wbuf(void)
{
    /* Bug repro: when the word-buffer is exactly at capacity and a
     * combining mark arrives, the mark must still emit *after* the
     * buffered glyphs flush — otherwise it attaches to whatever
     * glyph last committed before wbuf filled and its cursor report
     * uses the pre-flush column.
     *
     * Constructed input: WBUF_CAP 'a's so wbuf is full right when
     * the next byte is read, immediately followed by U+0301
     * (combining acute, 0xCC 0x81). cols=0 keeps the wrap path
     * dormant; the cap-overflow path is the only way wbuf flushes.
     *
     * WBUF_CAP is private to input_core.c; the constant below is a
     * copy. If the implementation cap changes this test must be
     * updated to match (the bug is at the cap boundary, not on
     * either side of it). */
    enum { LEAD = 256 };
    char buf[LEAD + 2 + 1];
    for (int i = 0; i < LEAD; i++)
        buf[i] = 'a';
    buf[LEAD] = (char)0xcc;
    buf[LEAD + 1] = (char)0x81;
    buf[LEAD + 2] = '\0';

    struct order_rec o = {0};
    input_core_render(buf, LEAD + 2, 0, 0, 0, 0, order_cb, &o, NULL);
    EXPECT(o.combining_seen);
    EXPECT(o.a_count == LEAD);
    EXPECT(o.combining_after_a_count == LEAD);
}

/* ---------- buffer ops ---------- */

static void test_buf_set_and_insert(void)
{
    struct input *in = new_with("hello");
    EXPECT(in->len == 5);
    EXPECT(in->cursor == 5);
    EXPECT_STR_EQ(in->buf, "hello");

    /* insert at end */
    input_core_buf_insert(in, "!", 1);
    EXPECT_STR_EQ(in->buf, "hello!");
    EXPECT(in->cursor == 6);

    /* insert at start */
    in->cursor = 0;
    input_core_buf_insert(in, ">>", 2);
    EXPECT_STR_EQ(in->buf, ">>hello!");
    EXPECT(in->cursor == 2);

    /* insert mid-buffer (the memmove-shift path) */
    in->cursor = 4; /* between ">>he" and "llo!" */
    input_core_buf_insert(in, "XX", 2);
    EXPECT_STR_EQ(in->buf, ">>heXXllo!");
    EXPECT(in->cursor == 6);

    /* set replaces */
    input_core_buf_set(in, "abc");
    EXPECT_STR_EQ(in->buf, "abc");
    EXPECT(in->len == 3);
    EXPECT(in->cursor == 3);

    /* set NULL → empty */
    input_core_buf_set(in, NULL);
    EXPECT(in->len == 0);
    EXPECT_STR_EQ(in->buf, "");

    input_free(in);
}

/* ---------- motions / edits ---------- */

static void test_motions_ascii(void)
{
    struct input *in = new_with("hi");
    EXPECT(in->cursor == 2);
    input_core_move_left(in);
    EXPECT(in->cursor == 1);
    input_core_move_left(in);
    EXPECT(in->cursor == 0);
    input_core_move_left(in); /* clamp */
    EXPECT(in->cursor == 0);
    input_core_move_right(in);
    EXPECT(in->cursor == 1);
    input_core_move_right(in);
    input_core_move_right(in); /* clamp */
    EXPECT(in->cursor == 2);
    input_free(in);
}

static void test_motions_malformed_utf8(void)
{
    /* "\xC3(" — 0xC3 claims a 2-byte sequence but '(' isn't a
     * continuation byte. The renderer treats each byte as a 1-col
     * substitute, so motion must step byte-by-byte too — otherwise
     * Right would skip past '(' and Delete would eat it. */
    struct input *in = new_with("\xc3(");
    EXPECT(in->len == 2);
    in->cursor = 0;
    input_core_move_right(in);
    EXPECT(in->cursor == 1);
    input_core_move_right(in);
    EXPECT(in->cursor == 2);
    input_free(in);

    /* "A\x80" — stray continuation byte. Left from end must stop at
     * each byte, not jump over 'A'. */
    in = new_with("A\x80");
    EXPECT(in->len == 2);
    in->cursor = 2;
    input_core_move_left(in);
    EXPECT(in->cursor == 1);
    input_core_move_left(in);
    EXPECT(in->cursor == 0);
    input_free(in);

    /* Structurally complete but strictly invalid sequences must also
     * step one byte at a time, since the renderer (mbrtowc) rejects
     * them and substitutes per-byte. Cases:
     *   "\xC0\x80"     — overlong NUL (2-byte form)
     *   "\xED\xA0\x80" — UTF-16 surrogate U+D800
     *   "\xF5\x80\x80\x80" — codepoint > U+10FFFF */
    in = new_with("\xc0\x80");
    EXPECT(in->len == 2);
    in->cursor = 0;
    input_core_move_right(in);
    EXPECT(in->cursor == 1);
    input_core_move_right(in);
    EXPECT(in->cursor == 2);
    input_core_move_left(in);
    EXPECT(in->cursor == 1);
    input_free(in);

    in = new_with("\xed\xa0\x80");
    EXPECT(in->len == 3);
    in->cursor = 0;
    input_core_move_right(in);
    EXPECT(in->cursor == 1);
    in->cursor = 3;
    input_core_move_left(in);
    EXPECT(in->cursor == 2);
    input_free(in);

    in = new_with("\xf5\x80\x80\x80");
    EXPECT(in->len == 4);
    in->cursor = 0;
    input_core_move_right(in);
    EXPECT(in->cursor == 1);
    in->cursor = 4;
    input_core_move_left(in);
    EXPECT(in->cursor == 3);
    input_free(in);
}

static void test_motions_utf8(void)
{
    /* "héllo" — é is 2 bytes (0xc3 0xa9). */
    struct input *in = new_with("h\xc3\xa9llo");
    EXPECT(in->len == 6);
    /* cursor at end (6); move_left should land at start of 'o'... actually
     * walking back: l-l-é-h. Each move_left = one codepoint. */
    EXPECT(in->cursor == 6);
    input_core_move_left(in); /* skip 'o' */
    EXPECT(in->cursor == 5);
    input_core_move_left(in); /* skip 'l' */
    EXPECT(in->cursor == 4);
    input_core_move_left(in); /* skip 'l' */
    EXPECT(in->cursor == 3);
    input_core_move_left(in); /* skip é (2 bytes) */
    EXPECT(in->cursor == 1);
    input_core_move_left(in); /* skip 'h' */
    EXPECT(in->cursor == 0);
    input_free(in);
}

static void test_line_start_end(void)
{
    struct input *in = new_with("abc\ndef\nghi");
    in->cursor = 5; /* between 'd' and 'e' on line 2 */
    EXPECT(input_core_line_start(in) == 4);
    EXPECT(input_core_line_end(in) == 7);

    in->cursor = 0;
    EXPECT(input_core_line_start(in) == 0);
    EXPECT(input_core_line_end(in) == 3);

    in->cursor = 11;
    EXPECT(input_core_line_start(in) == 8);
    EXPECT(input_core_line_end(in) == 11);
    input_free(in);
}

static void test_delete_back_fwd(void)
{
    struct input *in = new_with("hello");
    in->cursor = 3;
    input_core_delete_back(in); /* removes 'l' before cursor */
    EXPECT_STR_EQ(in->buf, "helo");
    EXPECT(in->cursor == 2);

    input_core_delete_fwd(in); /* removes 'l' at cursor */
    EXPECT_STR_EQ(in->buf, "heo");
    EXPECT(in->cursor == 2);

    /* delete_back at cursor=0 is a no-op */
    in->cursor = 0;
    input_core_delete_back(in);
    EXPECT_STR_EQ(in->buf, "heo");

    /* delete_fwd at end is a no-op */
    in->cursor = in->len;
    input_core_delete_fwd(in);
    EXPECT_STR_EQ(in->buf, "heo");
    input_free(in);
}

static void test_kill_word_back(void)
{
    struct input *in = new_with("hello world");
    in->cursor = 11;
    input_core_kill_word_back(in);
    EXPECT_STR_EQ(in->buf, "hello ");

    /* trailing whitespace + word */
    input_core_buf_set(in, "foo bar  ");
    in->cursor = 9;
    input_core_kill_word_back(in);
    EXPECT_STR_EQ(in->buf, "foo ");
    input_free(in);
}

static void test_move_word_left(void)
{
    struct input *in = new_with("foo bar  baz");
    in->cursor = 12;
    input_core_move_word_left(in);
    EXPECT(in->cursor == 9); /* start of "baz" */
    input_core_move_word_left(in);
    EXPECT(in->cursor == 4); /* start of "bar" */
    input_core_move_word_left(in);
    EXPECT(in->cursor == 0); /* start of "foo" */
    input_core_move_word_left(in);
    EXPECT(in->cursor == 0); /* clamp */

    /* mid-word jumps to start of current word */
    in->cursor = 6; /* inside "bar" */
    input_core_move_word_left(in);
    EXPECT(in->cursor == 4);

    /* trailing whitespace skipped first */
    input_core_buf_set(in, "foo   ");
    in->cursor = 6;
    input_core_move_word_left(in);
    EXPECT(in->cursor == 0);
    input_free(in);
}

static void test_move_word_right(void)
{
    struct input *in = new_with("foo bar  baz");
    in->cursor = 0;
    input_core_move_word_right(in);
    EXPECT(in->cursor == 3); /* end of "foo" */
    input_core_move_word_right(in);
    EXPECT(in->cursor == 7); /* end of "bar" */
    input_core_move_word_right(in);
    EXPECT(in->cursor == 12); /* end of "baz" */
    input_core_move_word_right(in);
    EXPECT(in->cursor == 12); /* clamp */

    /* leading whitespace skipped */
    in->cursor = 3; /* on space after "foo" */
    input_core_move_word_right(in);
    EXPECT(in->cursor == 7);
    input_free(in);
}

static void test_move_word_utf8(void)
{
    /* "héllo wörld" — multi-byte chars count as word bytes, so the
     * single ASCII space is the only boundary, and the cursor lands on
     * UTF-8 leader bytes. */
    struct input *in = new_with("héllo wörld");
    in->cursor = in->len;
    input_core_move_word_left(in);
    EXPECT(in->cursor == 7); /* start of "wörld" — 'h' + 'é'(2) + "llo " = 7 */
    input_core_move_word_left(in);
    EXPECT(in->cursor == 0);

    /* Punctuation breaks across UTF-8 letters too: "héllo/wörld" splits
     * at the slash. */
    input_core_buf_set(in, "héllo/wörld");
    in->cursor = in->len;
    input_core_move_word_left(in);
    EXPECT(in->cursor == 7); /* start of "wörld" past the slash */
    input_core_move_word_left(in);
    EXPECT(in->cursor == 0);
    input_free(in);
}

static void test_move_word_punctuation(void)
{
    /* Readline backward-word/forward-word treat any non-alnum byte as a
     * boundary, so common code/path tokens segment as expected. */
    struct input *in = new_with("foo/bar.baz");
    in->cursor = in->len;
    input_core_move_word_left(in);
    EXPECT(in->cursor == 8); /* start of "baz" */
    input_core_move_word_left(in);
    EXPECT(in->cursor == 4); /* start of "bar" */
    input_core_move_word_left(in);
    EXPECT(in->cursor == 0); /* start of "foo" */

    in->cursor = 0;
    input_core_move_word_right(in);
    EXPECT(in->cursor == 3); /* end of "foo" */
    input_core_move_word_right(in);
    EXPECT(in->cursor == 7); /* end of "bar" */
    input_core_move_word_right(in);
    EXPECT(in->cursor == 11); /* end of "baz" */

    /* Underscore is not alnum → it's a boundary, matching readline. */
    input_core_buf_set(in, "a_b_c");
    in->cursor = in->len;
    input_core_move_word_left(in);
    EXPECT(in->cursor == 4); /* "c" */
    input_core_move_word_left(in);
    EXPECT(in->cursor == 2); /* "b" */
    input_free(in);
}

static void test_kill_word_fwd(void)
{
    struct input *in = new_with("hello world");
    in->cursor = 0;
    input_core_kill_word_fwd(in);
    EXPECT_STR_EQ(in->buf, " world");
    EXPECT(in->cursor == 0);

    /* leading whitespace + word */
    input_core_buf_set(in, "foo  bar baz");
    in->cursor = 3;
    input_core_kill_word_fwd(in);
    EXPECT_STR_EQ(in->buf, "foo baz");

    /* punctuation is a boundary: M-d at start of "foo/bar" deletes
     * only "foo". */
    input_core_buf_set(in, "foo/bar");
    in->cursor = 0;
    input_core_kill_word_fwd(in);
    EXPECT_STR_EQ(in->buf, "/bar");

    /* at end of buffer is a no-op */
    input_core_buf_set(in, "abc");
    in->cursor = 3;
    input_core_kill_word_fwd(in);
    EXPECT_STR_EQ(in->buf, "abc");
    EXPECT(in->cursor == 3);
    input_free(in);
}

static void test_kill_word_back_alnum(void)
{
    /* Alt+Backspace stops at punctuation: "foo/bar|" deletes "bar"
     * only, leaving "foo/". Distinguishes from Ctrl-W which would
     * delete the whole "foo/bar" since it has no whitespace. */
    struct input *in = new_with("foo/bar");
    in->cursor = in->len;
    input_core_kill_word_back_alnum(in);
    EXPECT_STR_EQ(in->buf, "foo/");

    /* Ctrl-W on the same input deletes through the slash. */
    input_core_buf_set(in, "foo/bar");
    in->cursor = in->len;
    input_core_kill_word_back(in);
    EXPECT_STR_EQ(in->buf, "");

    /* Trailing punctuation is skipped first, like trailing whitespace
     * in Ctrl-W. "foo.bar..|" → "foo.". */
    input_core_buf_set(in, "foo.bar..");
    in->cursor = in->len;
    input_core_kill_word_back_alnum(in);
    EXPECT_STR_EQ(in->buf, "foo.");
    input_free(in);
}

static void test_kill_to_eol_joins_empty_line(void)
{
    /* On an empty logical line, Ctrl-K eats the \n (joins with next). */
    struct input *in = new_with("a\n\nb");
    in->cursor = 2; /* on the empty middle line */
    input_core_kill_to_eol(in);
    EXPECT_STR_EQ(in->buf, "a\nb");
    EXPECT(in->cursor == 2);
    input_free(in);
}

static void test_kill_to_eol_at_buffer_end(void)
{
    /* At end of buffer (no newline to eat, no chars to kill) Ctrl-K
     * is a no-op — must not walk past in->len. */
    struct input *in = new_with("abc");
    in->cursor = 3;
    input_core_kill_to_eol(in);
    EXPECT_STR_EQ(in->buf, "abc");
    EXPECT(in->cursor == 3);
    input_free(in);
}

static void test_kill_to_bol(void)
{
    struct input *in = new_with("hello world");
    in->cursor = 6;
    input_core_kill_to_bol(in);
    EXPECT_STR_EQ(in->buf, "world");
    EXPECT(in->cursor == 0);
    input_free(in);
}

/* ---------- history ---------- */

static void test_history_empty(void)
{
    struct input *in = input_new();
    input_core_history_prev(in); /* no-op */
    input_core_history_next(in); /* no-op */
    EXPECT(in->len == 0);
    input_free(in);
}

static void test_history_navigation(void)
{
    struct input *in = input_new();
    input_core_history_add(in, "one");
    input_core_history_add(in, "two");
    input_core_history_add(in, "three");
    EXPECT(in->hist_n == 3);
    EXPECT(in->hist_pos == 0); /* untouched at this point */

    /* Set pos to "current draft" position (what input_readline does on entry) */
    in->hist_pos = in->hist_n;
    input_core_buf_set(in, ""); /* empty draft */

    input_core_history_prev(in);
    EXPECT_STR_EQ(in->buf, "three");
    input_core_history_prev(in);
    EXPECT_STR_EQ(in->buf, "two");
    input_core_history_prev(in);
    EXPECT_STR_EQ(in->buf, "one");
    input_core_history_prev(in); /* clamp at oldest */
    EXPECT_STR_EQ(in->buf, "one");

    input_core_history_next(in);
    EXPECT_STR_EQ(in->buf, "two");
    input_core_history_next(in);
    EXPECT_STR_EQ(in->buf, "three");
    input_core_history_next(in); /* back to draft */
    EXPECT_STR_EQ(in->buf, "");
    input_core_history_next(in); /* clamp at draft */
    EXPECT_STR_EQ(in->buf, "");
    input_free(in);
}

static void test_history_draft_preserved(void)
{
    struct input *in = input_new();
    input_core_history_add(in, "older");
    in->hist_pos = in->hist_n;
    input_core_buf_set(in, "my draft");

    input_core_history_prev(in);
    EXPECT_STR_EQ(in->buf, "older");
    input_core_history_next(in);
    EXPECT_STR_EQ(in->buf, "my draft");
    input_free(in);
}

static void test_history_recall_edits_discarded(void)
{
    /* Editing a recalled entry is local; navigating away discards the edit
     * and the original entry stays unchanged. */
    struct input *in = input_new();
    input_core_history_add(in, "a");
    input_core_history_add(in, "b");
    in->hist_pos = in->hist_n;
    input_core_buf_set(in, "draft");

    input_core_history_prev(in);
    EXPECT_STR_EQ(in->buf, "b");
    input_core_buf_insert(in, "!", 1);
    EXPECT_STR_EQ(in->buf, "b!");

    input_core_history_prev(in);
    EXPECT_STR_EQ(in->buf, "a");

    input_core_history_next(in);
    EXPECT_STR_EQ(in->buf, "b"); /* the '!' edit is gone */

    input_core_history_next(in);
    EXPECT_STR_EQ(in->buf, "draft");
    input_free(in);
}

static void test_history_evicts_oldest(void)
{
    /* INPUT_CORE_HISTORY_MAX caps retained entries; the oldest one is
     * dropped when capacity is exceeded. Push cap+1 unique entries and
     * verify the first one is gone. */
    struct input *in = input_new();
    char buf[16];
    const int cap = INPUT_CORE_HISTORY_MAX;
    for (int i = 0; i <= cap; i++) {
        snprintf(buf, sizeof(buf), "e%d", i);
        input_core_history_add(in, buf);
    }
    EXPECT(in->hist_n == (size_t)cap);
    EXPECT_STR_EQ(in->hist[0], "e1"); /* oldest, "e0" evicted */
    snprintf(buf, sizeof(buf), "e%d", cap);
    EXPECT_STR_EQ(in->hist[cap - 1], buf); /* newest */
    input_free(in);
}

static void test_history_erasedups(void)
{
    /* Erasedups semantics: any prior exact match (anywhere, not just
     * the most recent) is removed before append, so a recalled entry
     * bumps to the top instead of duplicating. The same-as-most-recent
     * case is a fast-path skip (no work, return 0) so the persistence
     * wrapper can avoid an on-disk record. */
    struct input *in = input_new();
    EXPECT(input_core_history_add(in, "a") == 1);
    EXPECT(input_core_history_add(in, "a") == 0); /* skip: already most recent */
    EXPECT(in->hist_n == 1);
    EXPECT_STR_EQ(in->hist[0], "a");

    EXPECT(input_core_history_add(in, "b") == 1);
    EXPECT(input_core_history_add(in, "a") == 1); /* erases older "a" at idx 0 */
    EXPECT(in->hist_n == 2);
    EXPECT_STR_EQ(in->hist[0], "b");
    EXPECT_STR_EQ(in->hist[1], "a");

    /* Empty / NULL ignored, return 0. */
    EXPECT(input_core_history_add(in, "") == 0);
    EXPECT(input_core_history_add(in, NULL) == 0);
    EXPECT(in->hist_n == 2);
    input_free(in);
}

static void test_history_encode_decode_roundtrip(void)
{
    /* Plain ASCII passes through unchanged. */
    char *enc = input_core_history_encode("hello world");
    EXPECT_STR_EQ(enc, "hello world");
    char *dec = input_core_history_decode(enc, strlen(enc));
    EXPECT_STR_EQ(dec, "hello world");
    free(enc);
    free(dec);

    /* Embedded LF and backslashes are escaped on encode and restored. */
    const char *raw = "line1\nline2\\tab\n";
    enc = input_core_history_encode(raw);
    EXPECT_STR_EQ(enc, "line1\\nline2\\\\tab\\n");
    dec = input_core_history_decode(enc, strlen(enc));
    EXPECT_STR_EQ(dec, raw);
    free(enc);
    free(dec);

    /* Unknown escape preserves verbatim (forward compat). */
    dec = input_core_history_decode("a\\xb", 4);
    EXPECT_STR_EQ(dec, "a\\xb");
    free(dec);

    /* Trailing lone backslash preserved as-is. */
    dec = input_core_history_decode("end\\", 4);
    EXPECT_STR_EQ(dec, "end\\");
    free(dec);
}

/* ---------- prompt_width ---------- */

static void test_prompt_width_strips_ansi(void)
{
    EXPECT(input_core_prompt_width("> ") == 2);
    EXPECT(input_core_prompt_width("\x1b[35m\x1b[1m>\x1b[22m\x1b[39m ") == 2);
    EXPECT(input_core_prompt_width("") == 0);
}

/* ---------- decode_escape ---------- */

/* Byte-array reader for input_core_decode_escape. The decoder calls
 * `read` once per byte; the array is consumed left-to-right and -1 is
 * returned past the end (modeling EOF / timeout). */
struct ba_reader {
    const unsigned char *bytes;
    size_t pos, len;
};

static int read_ba(void *user)
{
    struct ba_reader *r = user;
    if (r->pos >= r->len)
        return -1;
    return r->bytes[r->pos++];
}

/* Decode the bytes following an ESC (the leading ESC is *not* part of
 * `seq`). Length is taken from a sentinel terminator: tests that need
 * embedded NULs or precise lengths use decode_n. */
static enum input_action decode(const char *seq)
{
    struct ba_reader r = {.bytes = (const unsigned char *)seq, .pos = 0, .len = strlen(seq)};
    return input_core_decode_escape(read_ba, &r);
}

static enum input_action decode_n(const unsigned char *seq, size_t n)
{
    struct ba_reader r = {.bytes = seq, .pos = 0, .len = n};
    return input_core_decode_escape(read_ba, &r);
}

static void test_decode_unmodified_csi(void)
{
    /* CSI A/B/C/D/H/F. Up/Down map to history navigation. */
    EXPECT(decode("[A") == INPUT_ACTION_HISTORY_PREV);
    EXPECT(decode("[B") == INPUT_ACTION_HISTORY_NEXT);
    EXPECT(decode("[C") == INPUT_ACTION_MOVE_RIGHT);
    EXPECT(decode("[D") == INPUT_ACTION_MOVE_LEFT);
    EXPECT(decode("[H") == INPUT_ACTION_LINE_START);
    EXPECT(decode("[F") == INPUT_ACTION_LINE_END);
}

static void test_decode_unmodified_ss3(void)
{
    EXPECT(decode("OA") == INPUT_ACTION_HISTORY_PREV);
    EXPECT(decode("OB") == INPUT_ACTION_HISTORY_NEXT);
    EXPECT(decode("OC") == INPUT_ACTION_MOVE_RIGHT);
    EXPECT(decode("OD") == INPUT_ACTION_MOVE_LEFT);
    EXPECT(decode("OH") == INPUT_ACTION_LINE_START);
    EXPECT(decode("OF") == INPUT_ACTION_LINE_END);
}

static void test_decode_xterm_modified_arrows(void)
{
    /* "1;<mod><letter>" — Alt/Ctrl/Meta-flavored arrows do word
     * motion; plain (mod=1) and Shift-only (mod=2) fall back to
     * single-char motion. */
    EXPECT(decode("[1;5D") == INPUT_ACTION_MOVE_WORD_LEFT);  /* Ctrl+Left */
    EXPECT(decode("[1;5C") == INPUT_ACTION_MOVE_WORD_RIGHT); /* Ctrl+Right */
    EXPECT(decode("[1;3D") == INPUT_ACTION_MOVE_WORD_LEFT);  /* Alt+Left */
    EXPECT(decode("[1;3C") == INPUT_ACTION_MOVE_WORD_RIGHT); /* Alt+Right */
    EXPECT(decode("[1;7D") == INPUT_ACTION_MOVE_WORD_LEFT);  /* Alt+Ctrl+Left */
    EXPECT(decode("[1;1D") == INPUT_ACTION_MOVE_LEFT);       /* no mod */
    EXPECT(decode("[1;2D") == INPUT_ACTION_MOVE_LEFT);       /* Shift only */
    EXPECT(decode("[1;5H") == INPUT_ACTION_LINE_START);      /* Ctrl+Home */
    EXPECT(decode("[1;5F") == INPUT_ACTION_LINE_END);
}

static void test_decode_kitty_extra_param(void)
{
    /* kitty/xterm modifyOtherKeys appends a key-event-type param:
     * "1;5;2D" should still decode as Ctrl+Left, not fall through. */
    EXPECT(decode("[1;5;2D") == INPUT_ACTION_MOVE_WORD_LEFT);
}

static void test_decode_xterm_modified_ss3(void)
{
    /* SS3 modified form: "ESC O 1;<mod><letter>". */
    EXPECT(decode("O1;5D") == INPUT_ACTION_MOVE_WORD_LEFT);
    EXPECT(decode("O1;5C") == INPUT_ACTION_MOVE_WORD_RIGHT);
    EXPECT(decode("O1;1D") == INPUT_ACTION_MOVE_LEFT);
}

static void test_decode_tilde_unmodified(void)
{
    /* Home/End/Delete in tilde encoding. Both code variants for
     * Home (1, 7) and End (4, 8). */
    EXPECT(decode("[1~") == INPUT_ACTION_LINE_START);
    EXPECT(decode("[7~") == INPUT_ACTION_LINE_START);
    EXPECT(decode("[4~") == INPUT_ACTION_LINE_END);
    EXPECT(decode("[8~") == INPUT_ACTION_LINE_END);
    EXPECT(decode("[3~") == INPUT_ACTION_DELETE_FWD);
}

static void test_decode_tilde_xterm_modified(void)
{
    /* xterm modified tilde: "<digit>;<mod>~". Modifier is irrelevant
     * for the keys we care about. */
    EXPECT(decode("[3;5~") == INPUT_ACTION_DELETE_FWD); /* Ctrl+Delete */
    EXPECT(decode("[7;5~") == INPUT_ACTION_LINE_START); /* Ctrl+Home */
}

static void test_decode_tilde_no_fkey_alias(void)
{
    /* F-keys (F5+) use multi-digit codes in tilde encoding. They must
     * NOT alias to the single-digit Home/End/Delete cases. */
    EXPECT(decode("[15~") == INPUT_ACTION_NONE); /* F5 — not Home */
    EXPECT(decode("[17~") == INPUT_ACTION_NONE); /* F6 */
    EXPECT(decode("[18~") == INPUT_ACTION_NONE); /* F7 */
    EXPECT(decode("[19~") == INPUT_ACTION_NONE); /* F8 */
    EXPECT(decode("[34~") == INPUT_ACTION_NONE); /* not End */
    EXPECT(decode("[31~") == INPUT_ACTION_NONE); /* not Delete */
}

static void test_decode_rxvt_csi_lowercase(void)
{
    /* rxvt encodes Shift+arrow as lowercase finals in CSI form.
     * Without selection support, plain motion is the right fallback. */
    EXPECT(decode("[a") == INPUT_ACTION_HISTORY_PREV);
    EXPECT(decode("[b") == INPUT_ACTION_HISTORY_NEXT);
    EXPECT(decode("[c") == INPUT_ACTION_MOVE_RIGHT);
    EXPECT(decode("[d") == INPUT_ACTION_MOVE_LEFT);
}

static void test_decode_rxvt_ss3_lowercase(void)
{
    /* rxvt encodes Ctrl+arrow as lowercase finals in SS3 form. */
    EXPECT(decode("Oa") == INPUT_ACTION_HISTORY_PREV);
    EXPECT(decode("Ob") == INPUT_ACTION_HISTORY_NEXT);
    EXPECT(decode("Oc") == INPUT_ACTION_MOVE_WORD_RIGHT);
    EXPECT(decode("Od") == INPUT_ACTION_MOVE_WORD_LEFT);
}

static void test_decode_rxvt_tilde_finals(void)
{
    /* rxvt modifier-encoded tilde finals: ^ Ctrl, $ Shift, @ both.
     * '$' (0x24) is below the standard final-byte range; the decoder
     * special-cases it as a terminator. Action is unchanged from the
     * unmodified form for these keys. */
    EXPECT(decode("[7^") == INPUT_ACTION_LINE_START); /* Ctrl+Home */
    EXPECT(decode("[8^") == INPUT_ACTION_LINE_END);   /* Ctrl+End */
    EXPECT(decode("[3^") == INPUT_ACTION_DELETE_FWD); /* Ctrl+Delete */
    EXPECT(decode("[7$") == INPUT_ACTION_LINE_START); /* Shift+Home */
    EXPECT(decode("[8$") == INPUT_ACTION_LINE_END);   /* Shift+End */
    EXPECT(decode("[3$") == INPUT_ACTION_DELETE_FWD); /* Shift+Delete */
    EXPECT(decode("[7@") == INPUT_ACTION_LINE_START); /* Ctrl+Shift+Home */
}

static void test_decode_iterm_meta_prefix(void)
{
    /* iTerm2 "Esc+" mode prepends an ESC to the inner sequence for
     * Alt-modified keys: "ESC ESC [ D" = Alt+Left = word motion. The
     * leading ESC is consumed by the caller, so the decoder sees the
     * second ESC at the start of its input. */
    EXPECT(decode("\x1b[D") == INPUT_ACTION_MOVE_WORD_LEFT);
    EXPECT(decode("\x1b[C") == INPUT_ACTION_MOVE_WORD_RIGHT);
    EXPECT(decode("\x1b[A") == INPUT_ACTION_HISTORY_PREV);

    /* Meta-prefix combined with an explicit Ctrl modifier still
     * produces word motion (the OR with `meta` doesn't downgrade). */
    EXPECT(decode("\x1b[1;5D") == INPUT_ACTION_MOVE_WORD_LEFT);

    /* Multiple ESCs are tolerated up to the strip cap (4 extras). */
    EXPECT(decode("\x1b\x1b\x1b[D") == INPUT_ACTION_MOVE_WORD_LEFT);

    /* Beyond the cap: still ESC after stripping → abandon. */
    EXPECT(decode("\x1b\x1b\x1b\x1b\x1b[D") == INPUT_ACTION_NONE);
}

static void test_decode_meta_letters(void)
{
    /* Bare-ESC + letter readline bindings (M-b, M-f, M-d). */
    EXPECT(decode("b") == INPUT_ACTION_MOVE_WORD_LEFT);
    EXPECT(decode("B") == INPUT_ACTION_MOVE_WORD_LEFT);
    EXPECT(decode("f") == INPUT_ACTION_MOVE_WORD_RIGHT);
    EXPECT(decode("F") == INPUT_ACTION_MOVE_WORD_RIGHT);
    EXPECT(decode("d") == INPUT_ACTION_KILL_WORD_FWD);
    EXPECT(decode("D") == INPUT_ACTION_KILL_WORD_FWD);

    /* Alt+Backspace, both encodings. Length-explicit since 0x08 can
     * confuse strlen on a string literal. */
    unsigned char del = 0x7f;
    unsigned char bs = 0x08;
    EXPECT(decode_n(&del, 1) == INPUT_ACTION_KILL_WORD_BACK_ALNUM);
    EXPECT(decode_n(&bs, 1) == INPUT_ACTION_KILL_WORD_BACK_ALNUM);

    /* Alt+Enter: ESC + CR or ESC + LF inserts a newline. */
    EXPECT(decode("\r") == INPUT_ACTION_INSERT_NEWLINE);
    EXPECT(decode("\n") == INPUT_ACTION_INSERT_NEWLINE);
}

static void test_decode_paste_begin(void)
{
    /* Bracketed paste start. The decoder returns PASTE_BEGIN; the IO
     * layer reads the body. */
    EXPECT(decode("[200~") == INPUT_ACTION_PASTE_BEGIN);
}

static void test_decode_partial_and_unknown(void)
{
    /* Bare ESC (empty input after the leading ESC) → NONE. */
    EXPECT(decode("") == INPUT_ACTION_NONE);

    /* Truncated CSI body (no final byte before EOF) → NONE. */
    EXPECT(decode("[1;5") == INPUT_ACTION_NONE);

    /* Unknown CSI final letter → NONE. */
    EXPECT(decode("[Z") == INPUT_ACTION_NONE); /* Shift-Tab */

    /* Unknown SS3 final → NONE. */
    EXPECT(decode("OP") == INPUT_ACTION_NONE); /* F1 */
}

static void test_decode_overflow_and_runaway(void)
{
    /* Buffer-overflow path: sequence longer than the internal seq
     * buffer (32) but shorter than the read cap (64). Decoder must
     * discard the excess and consume bytes through the final, not
     * leave them queued for the main loop to insert as text. */
    unsigned char ovf[42];
    ovf[0] = '[';
    for (int i = 1; i < 41; i++)
        ovf[i] = '5';
    ovf[41] = 'D';
    struct ba_reader r = {.bytes = ovf, .pos = 0, .len = sizeof(ovf)};
    EXPECT(input_core_decode_escape(read_ba, &r) == INPUT_ACTION_NONE);
    EXPECT(r.pos == sizeof(ovf)); /* final byte consumed, stream drained */

    /* Runaway path: sequence with no final byte before the read cap
     * (64). Decoder gives up; the remaining bytes are intentionally
     * left in the stream because we can't keep reading forever. */
    unsigned char run[100];
    run[0] = '[';
    for (size_t i = 1; i < sizeof(run); i++)
        run[i] = '5';
    EXPECT(decode_n(run, sizeof(run)) == INPUT_ACTION_NONE);
}

int main(void)
{
    /* mbrtowc inside compute_layout needs a UTF-8 LC_CTYPE for the
     * multi-byte codepath to be exercised. */
    locale_init_utf8();

    test_layout_empty();
    test_layout_single_line_cursor_positions();
    test_layout_multi_line();
    test_layout_buffer_ending_in_newline();
    test_layout_continuation_indent();
    test_layout_combining_marks();
    test_layout_c1_in_utf8();
    test_layout_bidi_substituted();
    test_layout_tabs();
    test_layout_soft_wrap_char();
    test_layout_soft_wrap_word();
    test_layout_soft_wrap_drops_boundary_space();
    test_layout_soft_wrap_cursor_after_wrap();

    test_render_word_wrap_emits_break();
    test_render_drops_boundary_space();
    test_render_char_wrap_fallback();
    test_render_overlong_word_after_wrap_char_wraps();
    test_render_hard_newline_indents();
    test_render_tab_expands();
    test_render_unsafe_substituted();
    test_render_combining_after_full_wbuf();

    test_buf_set_and_insert();
    test_motions_ascii();
    test_motions_utf8();
    test_motions_malformed_utf8();
    test_line_start_end();
    test_delete_back_fwd();
    test_kill_word_back();
    test_move_word_left();
    test_move_word_right();
    test_move_word_utf8();
    test_move_word_punctuation();
    test_kill_word_fwd();
    test_kill_word_back_alnum();
    test_kill_to_eol_joins_empty_line();
    test_kill_to_eol_at_buffer_end();
    test_kill_to_bol();

    test_history_empty();
    test_history_navigation();
    test_history_draft_preserved();
    test_history_recall_edits_discarded();
    test_history_evicts_oldest();
    test_history_erasedups();
    test_history_encode_decode_roundtrip();

    test_prompt_width_strips_ansi();

    test_decode_unmodified_csi();
    test_decode_unmodified_ss3();
    test_decode_xterm_modified_arrows();
    test_decode_kitty_extra_param();
    test_decode_xterm_modified_ss3();
    test_decode_tilde_unmodified();
    test_decode_tilde_xterm_modified();
    test_decode_tilde_no_fkey_alias();
    test_decode_rxvt_csi_lowercase();
    test_decode_rxvt_ss3_lowercase();
    test_decode_rxvt_tilde_finals();
    test_decode_iterm_meta_prefix();
    test_decode_meta_letters();
    test_decode_paste_begin();
    test_decode_partial_and_unknown();
    test_decode_overflow_and_runaway();

    T_REPORT();
}
