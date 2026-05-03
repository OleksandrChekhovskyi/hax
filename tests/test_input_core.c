/* SPDX-License-Identifier: MIT */
#include "input_core.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "input.h"

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

    /* Tab that would exceed cols: wrap to row 1 col 0, then advance
     * by TAB_WIDTH. */
    input_core_compute_layout("\t", 1, 1, 0, 2, &L);
    EXPECT(L.end_row == 1);
    EXPECT(L.end_col == 4);
}

static void test_layout_soft_wrap(void)
{
    /* prompt_w=2, cols=10, "0123456789" (10 chars). After 8 chars on row 0
     * (col 2..9), 9th char wraps to col 0 of row 1. */
    struct input_layout L;
    input_core_compute_layout("0123456789", 10, 10, 2, 10, &L);
    EXPECT(L.total_rows == 2);
    EXPECT(L.end_row == 1);
    /* On row 0 we fit 8 cols (10 - 2). Char at idx 8 wraps to row 1 col 0,
     * col advances by 1 per char, so end_col after 2 chars on row 1 = 2. */
    EXPECT(L.end_col == 2);
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

/* ---------- utf8_seq_len ---------- */

static void test_utf8_seq_len(void)
{
    EXPECT(input_core_utf8_seq_len('a') == 1);
    EXPECT(input_core_utf8_seq_len(0x7F) == 1);
    EXPECT(input_core_utf8_seq_len(0xC3) == 2); /* 2-byte leader */
    EXPECT(input_core_utf8_seq_len(0xE2) == 3); /* 3-byte leader */
    EXPECT(input_core_utf8_seq_len(0xF0) == 4); /* 4-byte leader */
    EXPECT(input_core_utf8_seq_len(0x80) == 1); /* malformed → 1 */
}

int main(void)
{
    /* mbrtowc inside compute_layout needs a UTF-8 LC_CTYPE for the multi-byte
     * codepath to be exercised. C.UTF-8 is widely available; en_US.UTF-8 as
     * a fallback for systems that lack C.UTF-8 (older macOS). */
    if (!setlocale(LC_CTYPE, "C.UTF-8"))
        setlocale(LC_CTYPE, "en_US.UTF-8");

    test_layout_empty();
    test_layout_single_line_cursor_positions();
    test_layout_multi_line();
    test_layout_buffer_ending_in_newline();
    test_layout_continuation_indent();
    test_layout_combining_marks();
    test_layout_c1_in_utf8();
    test_layout_bidi_substituted();
    test_layout_tabs();
    test_layout_soft_wrap();

    test_buf_set_and_insert();
    test_motions_ascii();
    test_motions_utf8();
    test_motions_malformed_utf8();
    test_line_start_end();
    test_delete_back_fwd();
    test_kill_word_back();
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
    test_utf8_seq_len();

    T_REPORT();
}
