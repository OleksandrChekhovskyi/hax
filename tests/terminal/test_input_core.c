/* SPDX-License-Identifier: MIT */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "system/locale.h"
#include "terminal/input.h"
#include "terminal/input_core.h"

static struct input *new_with(const char *initial)
{
    struct input *in = input_new();
    if (initial)
        input_core_set_buffer(in, initial);
    return in;
}

static void test_layout_empty(void)
{
    struct input_layout layout;
    input_core_compute_layout("", 0, 0, 2, 80, &layout);
    EXPECT(layout.cursor_row == 0);
    EXPECT(layout.cursor_col == 2);
    EXPECT(layout.end_row == 0);
    EXPECT(layout.end_col == 2);
    EXPECT(layout.total_rows == 1);
}

static void test_layout_single_line_cursor_positions(void)
{
    struct input_layout layout;
    input_core_compute_layout("hello", 5, 0, 2, 80, &layout);
    EXPECT(layout.cursor_row == 0);
    EXPECT(layout.cursor_col == 2);
    EXPECT(layout.end_col == 7);
    EXPECT(layout.total_rows == 1);

    input_core_compute_layout("hello", 5, 3, 2, 80, &layout);
    EXPECT(layout.cursor_col == 5);

    input_core_compute_layout("hello", 5, 5, 2, 80, &layout);
    EXPECT(layout.cursor_col == 7);
}

static void test_layout_multi_line(void)
{
    struct input_layout layout;
    const char *s = "ab\ncd";

    input_core_compute_layout(s, 5, 0, 2, 80, &layout);
    EXPECT(layout.cursor_row == 0 && layout.cursor_col == 2);

    input_core_compute_layout(s, 5, 2, 2, 80, &layout);
    EXPECT(layout.cursor_row == 0 && layout.cursor_col == 4);

    input_core_compute_layout(s, 5, 3, 2, 80, &layout);
    EXPECT(layout.cursor_row == 1 && layout.cursor_col == 2);

    input_core_compute_layout(s, 5, 5, 2, 80, &layout);
    EXPECT(layout.cursor_row == 1 && layout.cursor_col == 4);
    EXPECT(layout.total_rows == 2);
}

static void test_layout_buffer_ending_in_newline(void)
{
    struct input_layout layout;
    input_core_compute_layout("ab\n", 3, 3, 2, 80, &layout);
    EXPECT(layout.cursor_row == 1);
    EXPECT(layout.cursor_col == 2);
    EXPECT(layout.total_rows == 2);
}

static void test_layout_continuation_indent(void)
{
    struct input_layout layout;
    input_core_compute_layout("a\nb\nc", 5, 5, 2, 80, &layout);
    EXPECT(layout.cursor_row == 2);
    EXPECT(layout.cursor_col == 3);
    EXPECT(layout.total_rows == 3);
}

static void test_layout_bidi_substituted(void)
{
    /* U+202E must occupy the renderer's substitute cell, preventing bidi reordering. */
    struct input_layout layout;
    input_core_compute_layout("\xe2\x80\xae", 3, 3, 0, 80, &layout);
    EXPECT(layout.end_col == 1);

    /* U+061C Arabic Letter Mark */
    input_core_compute_layout("\xd8\x9c", 2, 2, 0, 80, &layout);
    EXPECT(layout.end_col == 1);

    /* U+2060 word joiner */
    input_core_compute_layout("\xe2\x81\xa0", 3, 3, 0, 80, &layout);
    EXPECT(layout.end_col == 1);
}

static void test_layout_c1_in_utf8(void)
{
    /* U+009B must occupy its substitute cell rather than act as C1 CSI. */
    struct input_layout layout;
    input_core_compute_layout("\xc2\x9b", 2, 2, 0, 80, &layout);
    EXPECT(layout.end_col == 1);
    EXPECT(layout.cursor_col == 1);
    EXPECT(layout.total_rows == 1);
}

static void test_layout_combining_marks(void)
{
    /* U+0301 is zero-width, so the cursor remains after its host cell. */
    struct input_layout layout;
    input_core_compute_layout("e\xcc\x81", 3, 3, 0, 80, &layout);
    EXPECT(layout.end_col == 1);
    EXPECT(layout.cursor_col == 1);
    EXPECT(layout.total_rows == 1);
}

static void test_layout_tabs(void)
{
    struct input_layout layout;
    input_core_compute_layout("\tx", 2, 2, 2, 80, &layout);
    EXPECT(layout.cursor_row == 0);
    EXPECT(layout.cursor_col == 7);
    EXPECT(layout.end_col == 7);

    input_core_compute_layout("\tx", 2, 0, 2, 80, &layout);
    EXPECT(layout.cursor_col == 2);

    input_core_compute_layout("\t\t", 2, 2, 2, 80, &layout);
    EXPECT(layout.end_col == 10);

    input_core_compute_layout("\t", 1, 1, 0, 2, &layout);
    EXPECT(layout.end_row == 0);
    EXPECT(layout.end_col == 4);
}

static void test_layout_soft_wrap_char(void)
{
    struct input_layout layout;
    input_core_compute_layout("0123456789", 10, 10, 2, 10, &layout);
    EXPECT(layout.total_rows == 2);
    EXPECT(layout.end_row == 1);
    EXPECT(layout.end_col == 5);
}

static void test_layout_soft_wrap_word(void)
{
    struct input_layout layout;
    input_core_compute_layout("hello world", 11, 11, 2, 10, &layout);
    EXPECT(layout.total_rows == 2);
    EXPECT(layout.end_row == 1);
    EXPECT(layout.end_col == 7);
}

static void test_layout_soft_wrap_drops_boundary_space(void)
{
    /* The reserved last cell turns the boundary space into a dropped wrap point. */
    struct input_layout layout;
    input_core_compute_layout("abcdefg ij", 10, 10, 2, 10, &layout);
    EXPECT(layout.total_rows == 2);
    EXPECT(layout.end_row == 1);
    EXPECT(layout.end_col == 4);
}

static void test_layout_soft_wrap_cursor_after_wrap(void)
{
    struct input_layout layout;
    input_core_compute_layout("hello world", 11, 6, 2, 10, &layout);
    EXPECT(layout.cursor_row == 1);
    EXPECT(layout.cursor_col == 2);

    input_core_compute_layout("hello world", 11, 8, 2, 10, &layout);
    EXPECT(layout.cursor_row == 1);
    EXPECT(layout.cursor_col == 4);
}

struct render_recording {
    char output[256];
    size_t len;
};

static void record_render_event(const struct input_render_event *event, void *user)
{
    struct render_recording *recording = user;

    if (event->kind == INPUT_RENDER_ROW_BREAK) {
        if (recording->len < sizeof(recording->output) - 1)
            recording->output[recording->len++] = '/';
        return;
    }
    for (size_t i = 0; i < event->n && recording->len < sizeof(recording->output) - 1; i++)
        recording->output[recording->len++] = event->bytes[i];
}

static void test_render_word_wrap_emits_break(void)
{
    struct render_recording recording = {0};
    input_core_render("hello world", 11, 0, 2, 2, 10, record_render_event, &recording, NULL);
    recording.output[recording.len] = '\0';
    EXPECT_STR_EQ(recording.output, "hello /world");
}

static void test_render_drops_boundary_space(void)
{
    struct render_recording recording = {0};
    input_core_render("abcdefg ij", 10, 0, 2, 2, 10, record_render_event, &recording, NULL);
    recording.output[recording.len] = '\0';
    EXPECT_STR_EQ(recording.output, "abcdefg/ij");
}

static void test_render_char_wrap_fallback(void)
{
    struct render_recording recording = {0};
    input_core_render("abcdefghijk", 11, 0, 2, 2, 10, record_render_event, &recording, NULL);
    recording.output[recording.len] = '\0';
    EXPECT_STR_EQ(recording.output, "abcdefg/hijk");
}

static void test_render_overlong_word_after_wrap_char_wraps(void)
{
    struct render_recording recording = {0};
    input_core_render("hello abcdefghijk", 17, 0, 2, 2, 10, record_render_event, &recording, NULL);
    recording.output[recording.len] = '\0';
    EXPECT_STR_EQ(recording.output, "hello /abcdefg/hijk");
}

static void test_render_hard_newline_indents(void)
{
    struct render_recording recording = {0};
    input_core_render("a\nb", 3, 0, 2, 2, 80, record_render_event, &recording, NULL);
    recording.output[recording.len] = '\0';
    EXPECT_STR_EQ(recording.output, "a/b");
}

static void test_render_tab_expands(void)
{
    struct render_recording recording = {0};
    input_core_render("a\tb", 3, 0, 0, 0, 80, record_render_event, &recording, NULL);
    recording.output[recording.len] = '\0';
    EXPECT_STR_EQ(recording.output, "a    b");
}

static void test_render_unsafe_substituted(void)
{
    struct render_recording recording = {0};
    input_core_render("a\x07"
                      "b",
                      3, 0, 0, 0, 80, record_render_event, &recording, NULL);
    recording.output[recording.len] = '\0';
    EXPECT_STR_EQ(recording.output, "a?b");
}

/* Record whether a combining mark follows every buffered host glyph. */
struct render_order {
    int a_count;
    int combining_after_a_count;
    int combining_seen;
};

static void record_render_order(const struct input_render_event *event, void *user)
{
    struct render_order *order = user;

    if (event->kind != INPUT_RENDER_GLYPH)
        return;
    if (event->n == 1 && event->bytes[0] == 'a') {
        order->a_count++;
    } else if (!order->combining_seen && event->width == 0) {
        order->combining_after_a_count = order->a_count;
        order->combining_seen = 1;
    }
}

static void test_render_combining_after_full_pending_buffer(void)
{
    /* Keep this boundary aligned with the renderer's private pending capacity. */
    enum { PENDING_CAPACITY = 256 };
    char text[PENDING_CAPACITY + 3];

    for (int i = 0; i < PENDING_CAPACITY; i++)
        text[i] = 'a';
    text[PENDING_CAPACITY] = (char)0xcc;
    text[PENDING_CAPACITY + 1] = (char)0x81;
    text[PENDING_CAPACITY + 2] = '\0';

    struct render_order order = {0};
    input_core_render(text, PENDING_CAPACITY + 2, 0, 0, 0, 0, record_render_order, &order, NULL);
    EXPECT(order.combining_seen);
    EXPECT(order.a_count == PENDING_CAPACITY);
    EXPECT(order.combining_after_a_count == PENDING_CAPACITY);
}

static void test_window_top_fits(void)
{
    EXPECT(input_core_window_top(0, 3, 5, 10) == 0);
    EXPECT(input_core_window_top(7, 3, 5, 10) == 0);
}

static void test_window_top_slides(void)
{
    EXPECT(input_core_window_top(0, 0, 30, 10) == 0);
    EXPECT(input_core_window_top(0, 9, 30, 10) == 0);
    EXPECT(input_core_window_top(0, 10, 30, 10) == 1);
    EXPECT(input_core_window_top(0, 29, 30, 10) == 20);
    EXPECT(input_core_window_top(5, 8, 30, 10) == 5);
    EXPECT(input_core_window_top(20, 19, 30, 10) == 19);
    EXPECT(input_core_window_top(25, 29, 30, 10) == 20);
}

static void test_render_window_hard_lines(void)
{
    struct render_recording recording = {0};
    struct input_layout layout;
    input_core_render_window("a\nb\nc\nd\ne", 9, 9, 0, 0, 80, 1, 3, record_render_event, &recording,
                             &layout);
    recording.output[recording.len] = '\0';
    EXPECT_STR_EQ(recording.output, "/b/c/d");
    EXPECT(layout.total_rows == 5);
    EXPECT(layout.cursor_row == 4);
}

static void test_render_window_from_top(void)
{
    struct render_recording recording = {0};
    input_core_render_window("a\nb\nc", 5, 0, 0, 0, 80, 0, 1, record_render_event, &recording,
                             NULL);
    recording.output[recording.len] = '\0';
    EXPECT_STR_EQ(recording.output, "a/b");
}

static void test_render_window_soft_wrap_rows(void)
{
    struct render_recording recording = {0};
    input_core_render_window("hello world", 11, 0, 2, 2, 10, 1, 1, record_render_event, &recording,
                             NULL);
    recording.output[recording.len] = '\0';
    EXPECT_STR_EQ(recording.output, "/world");
}

static void test_set_buffer_and_insert(void)
{
    struct input *in = new_with("hello");
    EXPECT(in->len == 5);
    EXPECT(in->cursor == 5);
    EXPECT_STR_EQ(in->buf, "hello");

    input_core_insert(in, "!", 1);
    EXPECT_STR_EQ(in->buf, "hello!");
    EXPECT(in->cursor == 6);

    in->cursor = 0;
    input_core_insert(in, ">>", 2);
    EXPECT_STR_EQ(in->buf, ">>hello!");
    EXPECT(in->cursor == 2);

    in->cursor = 4;
    input_core_insert(in, "XX", 2);
    EXPECT_STR_EQ(in->buf, ">>heXXllo!");
    EXPECT(in->cursor == 6);

    input_core_set_buffer(in, "abc");
    EXPECT_STR_EQ(in->buf, "abc");
    EXPECT(in->len == 3);
    EXPECT(in->cursor == 3);

    input_core_set_buffer(in, NULL);
    EXPECT(in->len == 0);
    EXPECT_STR_EQ(in->buf, "");

    input_free(in);
}

static void test_replace_span(void)
{
    struct input *in = new_with("see @src/m tail");

    input_core_replace_span(in, 4, 10, "src/main.c");
    EXPECT_STR_EQ(in->buf, "see src/main.c tail");
    EXPECT(in->cursor == 14);

    input_core_replace_span(in, 3, 14, NULL);
    EXPECT_STR_EQ(in->buf, "see tail");
    EXPECT(in->cursor == 3);

    input_core_set_buffer(in, "@x");
    input_core_replace_span(in, 0, 2, "a/very/long/replacement/path/that/needs/room.c");
    EXPECT_STR_EQ(in->buf, "a/very/long/replacement/path/that/needs/room.c");
    EXPECT(in->cursor == in->len);

    input_core_set_buffer(in, "abc");
    input_core_replace_span(in, 2, 1, "X");
    input_core_replace_span(in, 0, 4, "X");
    EXPECT_STR_EQ(in->buf, "abc");

    input_free(in);
}

static char *upcase_uri_filter(const char *text, void *user)
{
    (void)user;
    if (!strstr(text, "uri"))
        return NULL;
    char *out = strdup(text);
    for (char *p = out; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    return out;
}

static void test_commit_paste(void)
{
    struct input *in = new_with("");

    input_core_commit_paste(in, "plain", 5);
    EXPECT_STR_EQ(in->buf, "plain");

    input_core_set_buffer(in, "");
    in->paste_filter = upcase_uri_filter;
    input_core_commit_paste(in, "plain", 5);
    EXPECT_STR_EQ(in->buf, "plain");

    input_core_set_buffer(in, "see  here");
    in->cursor = 4;
    input_core_commit_paste(in, "uri", 3);
    EXPECT_STR_EQ(in->buf, "see URI here");
    EXPECT(in->cursor == 7);

    input_free(in);
}

static void test_motions_ascii(void)
{
    struct input *in = new_with("hi");
    EXPECT(in->cursor == 2);
    input_core_move_left(in);
    EXPECT(in->cursor == 1);
    input_core_move_left(in);
    EXPECT(in->cursor == 0);
    input_core_move_left(in);
    EXPECT(in->cursor == 0);
    input_core_move_right(in);
    EXPECT(in->cursor == 1);
    input_core_move_right(in);
    input_core_move_right(in);
    EXPECT(in->cursor == 2);
    input_free(in);
}

static void test_motions_malformed_utf8(void)
{
    /* Malformed "\xC3(" renders byte-wise, so motion must also advance byte-wise. */
    struct input *in = new_with("\xc3(");
    EXPECT(in->len == 2);
    in->cursor = 0;
    input_core_move_right(in);
    EXPECT(in->cursor == 1);
    input_core_move_right(in);
    EXPECT(in->cursor == 2);
    input_free(in);

    /* A stray continuation byte must not make backward motion skip the preceding ASCII. */
    in = new_with("A\x80");
    EXPECT(in->len == 2);
    in->cursor = 2;
    input_core_move_left(in);
    EXPECT(in->cursor == 1);
    input_core_move_left(in);
    EXPECT(in->cursor == 0);
    input_free(in);

    /* mbrtowc rejects overlong encodings, surrogates, and out-of-range codepoints; motion
     * must mirror the renderer's byte-wise substitution. */
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
    struct input *in = new_with("h\xc3\xa9llo");
    EXPECT(in->len == 6);

    EXPECT(in->cursor == 6);
    input_core_move_left(in);
    EXPECT(in->cursor == 5);
    input_core_move_left(in);
    EXPECT(in->cursor == 4);
    input_core_move_left(in);
    EXPECT(in->cursor == 3);
    input_core_move_left(in);
    EXPECT(in->cursor == 1);
    input_core_move_left(in);
    EXPECT(in->cursor == 0);
    input_free(in);
}

static void test_line_start_end(void)
{
    struct input *in = new_with("abc\ndef\nghi");
    in->cursor = 5;
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
    input_core_delete_back(in);
    EXPECT_STR_EQ(in->buf, "helo");
    EXPECT(in->cursor == 2);

    input_core_delete_fwd(in);
    EXPECT_STR_EQ(in->buf, "heo");
    EXPECT(in->cursor == 2);

    in->cursor = 0;
    input_core_delete_back(in);
    EXPECT_STR_EQ(in->buf, "heo");

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

    input_core_set_buffer(in, "foo bar  ");
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
    EXPECT(in->cursor == 9);
    input_core_move_word_left(in);
    EXPECT(in->cursor == 4);
    input_core_move_word_left(in);
    EXPECT(in->cursor == 0);
    input_core_move_word_left(in);
    EXPECT(in->cursor == 0);

    in->cursor = 6;
    input_core_move_word_left(in);
    EXPECT(in->cursor == 4);

    input_core_set_buffer(in, "foo   ");
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
    EXPECT(in->cursor == 3);
    input_core_move_word_right(in);
    EXPECT(in->cursor == 7);
    input_core_move_word_right(in);
    EXPECT(in->cursor == 12);
    input_core_move_word_right(in);
    EXPECT(in->cursor == 12);

    in->cursor = 3;
    input_core_move_word_right(in);
    EXPECT(in->cursor == 7);
    input_free(in);
}

static void test_move_word_utf8(void)
{
    struct input *in = new_with("héllo wörld");
    in->cursor = in->len;
    input_core_move_word_left(in);
    EXPECT(in->cursor == 7);
    input_core_move_word_left(in);
    EXPECT(in->cursor == 0);

    input_core_set_buffer(in, "héllo/wörld");
    in->cursor = in->len;
    input_core_move_word_left(in);
    EXPECT(in->cursor == 7);
    input_core_move_word_left(in);
    EXPECT(in->cursor == 0);
    input_free(in);
}

static void test_move_word_punctuation(void)
{
    struct input *in = new_with("foo/bar.baz");
    in->cursor = in->len;
    input_core_move_word_left(in);
    EXPECT(in->cursor == 8);
    input_core_move_word_left(in);
    EXPECT(in->cursor == 4);
    input_core_move_word_left(in);
    EXPECT(in->cursor == 0);

    in->cursor = 0;
    input_core_move_word_right(in);
    EXPECT(in->cursor == 3);
    input_core_move_word_right(in);
    EXPECT(in->cursor == 7);
    input_core_move_word_right(in);
    EXPECT(in->cursor == 11);

    input_core_set_buffer(in, "a_b_c");
    in->cursor = in->len;
    input_core_move_word_left(in);
    EXPECT(in->cursor == 4);
    input_core_move_word_left(in);
    EXPECT(in->cursor == 2);
    input_free(in);
}

static void test_kill_word_fwd(void)
{
    struct input *in = new_with("hello world");
    in->cursor = 0;
    input_core_kill_word_fwd(in);
    EXPECT_STR_EQ(in->buf, " world");
    EXPECT(in->cursor == 0);

    input_core_set_buffer(in, "foo  bar baz");
    in->cursor = 3;
    input_core_kill_word_fwd(in);
    EXPECT_STR_EQ(in->buf, "foo baz");

    input_core_set_buffer(in, "foo/bar");
    in->cursor = 0;
    input_core_kill_word_fwd(in);
    EXPECT_STR_EQ(in->buf, "/bar");

    input_core_set_buffer(in, "abc");
    in->cursor = 3;
    input_core_kill_word_fwd(in);
    EXPECT_STR_EQ(in->buf, "abc");
    EXPECT(in->cursor == 3);
    input_free(in);
}

static void test_kill_word_back_alnum(void)
{
    /* Alt+Backspace uses punctuation boundaries, unlike whitespace-based Ctrl-W. */
    struct input *in = new_with("foo/bar");
    in->cursor = in->len;
    input_core_kill_word_back_alnum(in);
    EXPECT_STR_EQ(in->buf, "foo/");

    input_core_set_buffer(in, "foo/bar");
    in->cursor = in->len;
    input_core_kill_word_back(in);
    EXPECT_STR_EQ(in->buf, "");

    input_core_set_buffer(in, "foo.bar..");
    in->cursor = in->len;
    input_core_kill_word_back_alnum(in);
    EXPECT_STR_EQ(in->buf, "foo.");
    input_free(in);
}

static void test_kill_to_eol_joins_empty_line(void)
{
    struct input *in = new_with("a\n\nb");
    in->cursor = 2;
    input_core_kill_to_eol(in);
    EXPECT_STR_EQ(in->buf, "a\nb");
    EXPECT(in->cursor == 2);
    input_free(in);
}

static void test_kill_to_eol_at_buffer_end(void)
{
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

static void test_history_empty(void)
{
    struct input *in = input_new();
    input_core_history_prev(in);
    input_core_history_next(in);
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
    EXPECT(in->hist_pos == 0);

    in->hist_pos = in->hist_n;
    input_core_set_buffer(in, "");

    input_core_history_prev(in);
    EXPECT_STR_EQ(in->buf, "three");
    input_core_history_prev(in);
    EXPECT_STR_EQ(in->buf, "two");
    input_core_history_prev(in);
    EXPECT_STR_EQ(in->buf, "one");
    input_core_history_prev(in);
    EXPECT_STR_EQ(in->buf, "one");

    input_core_history_next(in);
    EXPECT_STR_EQ(in->buf, "two");
    input_core_history_next(in);
    EXPECT_STR_EQ(in->buf, "three");
    input_core_history_next(in);
    EXPECT_STR_EQ(in->buf, "");
    input_core_history_next(in);
    EXPECT_STR_EQ(in->buf, "");
    input_free(in);
}

static void test_history_draft_preserved(void)
{
    struct input *in = input_new();
    input_core_history_add(in, "older");
    in->hist_pos = in->hist_n;
    input_core_set_buffer(in, "my draft");

    input_core_history_prev(in);
    EXPECT_STR_EQ(in->buf, "older");
    input_core_history_next(in);
    EXPECT_STR_EQ(in->buf, "my draft");
    input_free(in);
}

static void test_history_recall_edits_discarded(void)
{
    struct input *in = input_new();
    input_core_history_add(in, "a");
    input_core_history_add(in, "b");
    in->hist_pos = in->hist_n;
    input_core_set_buffer(in, "draft");

    input_core_history_prev(in);
    EXPECT_STR_EQ(in->buf, "b");
    input_core_insert(in, "!", 1);
    EXPECT_STR_EQ(in->buf, "b!");

    input_core_history_prev(in);
    EXPECT_STR_EQ(in->buf, "a");

    input_core_history_next(in);
    EXPECT_STR_EQ(in->buf, "b");

    input_core_history_next(in);
    EXPECT_STR_EQ(in->buf, "draft");
    input_free(in);
}

static void test_history_evicts_oldest(void)
{
    struct input *in = input_new();
    char buf[16];
    const int cap = INPUT_CORE_HISTORY_MAX;
    for (int i = 0; i <= cap; i++) {
        snprintf(buf, sizeof(buf), "e%d", i);
        input_core_history_add(in, buf);
    }
    EXPECT(in->hist_n == (size_t)cap);
    EXPECT_STR_EQ(in->hist[0], "e1");
    snprintf(buf, sizeof(buf), "e%d", cap);
    EXPECT_STR_EQ(in->hist[cap - 1], buf);
    input_free(in);
}

static void test_history_erasedups(void)
{
    struct input *in = input_new();
    EXPECT(input_core_history_add(in, "a") == 1);
    EXPECT(input_core_history_add(in, "a") == 0);
    EXPECT(in->hist_n == 1);
    EXPECT_STR_EQ(in->hist[0], "a");

    EXPECT(input_core_history_add(in, "b") == 1);
    EXPECT(input_core_history_add(in, "a") == 1);
    EXPECT(in->hist_n == 2);
    EXPECT_STR_EQ(in->hist[0], "b");
    EXPECT_STR_EQ(in->hist[1], "a");

    EXPECT(input_core_history_add(in, "") == 0);
    EXPECT(input_core_history_add(in, NULL) == 0);
    EXPECT(in->hist_n == 2);
    input_free(in);
}

static void test_history_search(void)
{
    struct input *in = input_new();
    input_core_history_add(in, "git status");
    input_core_history_add(in, "make build");
    input_core_history_add(in, "git commit -m wip");
    input_core_history_add(in, "make test");

    EXPECT(input_core_history_search(in, "git", (long)in->hist_n - 1, -1) == 2);

    EXPECT(input_core_history_search(in, "git", 1, -1) == 0);

    EXPECT(input_core_history_search(in, "git", -1, -1) == -1);

    EXPECT(input_core_history_search(in, "git", 0, 1) == 0);
    EXPECT(input_core_history_search(in, "git", 1, 1) == 2);

    EXPECT(input_core_history_search(in, "git", 3, 1) == -1);

    EXPECT(input_core_history_search(in, "test", (long)in->hist_n - 1, -1) == 3);
    EXPECT(input_core_history_search(in, "TEST", (long)in->hist_n - 1, -1) == -1);

    EXPECT(input_core_history_search(in, "", (long)in->hist_n - 1, -1) == -1);
    EXPECT(input_core_history_search(in, NULL, (long)in->hist_n - 1, -1) == -1);
    EXPECT(input_core_history_search(in, "git", (long)in->hist_n - 1, 0) == -1);
    EXPECT(input_core_history_search(in, "git", 99, -1) == -1);
    input_free(in);

    struct input *empty = input_new();
    EXPECT(input_core_history_search(empty, "x", 0, -1) == -1);
    input_free(empty);
}

static void test_history_encode_decode_roundtrip(void)
{
    char *enc = input_core_history_encode("hello world");
    EXPECT_STR_EQ(enc, "hello world");
    char *dec = input_core_history_decode(enc, strlen(enc));
    EXPECT_STR_EQ(dec, "hello world");
    free(enc);
    free(dec);

    const char *raw = "line1\nline2\\tab\n";
    enc = input_core_history_encode(raw);
    EXPECT_STR_EQ(enc, "line1\\nline2\\\\tab\\n");
    dec = input_core_history_decode(enc, strlen(enc));
    EXPECT_STR_EQ(dec, raw);
    free(enc);
    free(dec);

    dec = input_core_history_decode("a\\xb", 4);
    EXPECT_STR_EQ(dec, "a\\xb");
    free(dec);

    dec = input_core_history_decode("end\\", 4);
    EXPECT_STR_EQ(dec, "end\\");
    free(dec);
}

static void test_prompt_width_strips_ansi(void)
{
    EXPECT(input_core_prompt_width("> ") == 2);
    EXPECT(input_core_prompt_width("\x1b[35m\x1b[1m>\x1b[22m\x1b[39m ") == 2);
    EXPECT(input_core_prompt_width("") == 0);
}

struct byte_array_reader {
    const unsigned char *bytes;
    size_t position;
    size_t len;
};

static int read_byte_array(void *user)
{
    struct byte_array_reader *reader = user;

    if (reader->position >= reader->len)
        return -1;
    return reader->bytes[reader->position++];
}

static enum input_action decode(const char *sequence)
{
    struct byte_array_reader reader = {
        .bytes = (const unsigned char *)sequence,
        .len = strlen(sequence),
    };
    return input_core_decode_escape(read_byte_array, &reader);
}

static enum input_action decode_bytes(const unsigned char *sequence, size_t len)
{
    struct byte_array_reader reader = {.bytes = sequence, .len = len};
    return input_core_decode_escape(read_byte_array, &reader);
}

static void test_decode_unmodified_csi(void)
{
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
    /* Ignore kitty's trailing key-event parameter. */
    EXPECT(decode("[1;5;2D") == INPUT_ACTION_MOVE_WORD_LEFT);
}

static void test_decode_xterm_modified_ss3(void)
{
    EXPECT(decode("O1;5D") == INPUT_ACTION_MOVE_WORD_LEFT);
    EXPECT(decode("O1;5C") == INPUT_ACTION_MOVE_WORD_RIGHT);
    EXPECT(decode("O1;1D") == INPUT_ACTION_MOVE_LEFT);
}

static void test_decode_tilde_unmodified(void)
{
    EXPECT(decode("[1~") == INPUT_ACTION_LINE_START);
    EXPECT(decode("[7~") == INPUT_ACTION_LINE_START);
    EXPECT(decode("[4~") == INPUT_ACTION_LINE_END);
    EXPECT(decode("[8~") == INPUT_ACTION_LINE_END);
    EXPECT(decode("[3~") == INPUT_ACTION_DELETE_FWD);
    EXPECT(decode("[5~") == INPUT_ACTION_PAGE_UP);
    EXPECT(decode("[6~") == INPUT_ACTION_PAGE_DOWN);
}

static void test_decode_tilde_xterm_modified(void)
{
    EXPECT(decode("[3;5~") == INPUT_ACTION_DELETE_FWD); /* Ctrl+Delete */
    EXPECT(decode("[7;5~") == INPUT_ACTION_LINE_START); /* Ctrl+Home */
    EXPECT(decode("[5;5~") == INPUT_ACTION_PAGE_UP);    /* Ctrl+PageUp */
    EXPECT(decode("[6;5~") == INPUT_ACTION_PAGE_DOWN);  /* Ctrl+PageDown */
}

static void test_decode_tilde_no_fkey_alias(void)
{
    EXPECT(decode("[15~") == INPUT_ACTION_NONE);
    EXPECT(decode("[17~") == INPUT_ACTION_NONE);
    EXPECT(decode("[18~") == INPUT_ACTION_NONE);
    EXPECT(decode("[19~") == INPUT_ACTION_NONE);
    EXPECT(decode("[34~") == INPUT_ACTION_NONE);
    EXPECT(decode("[31~") == INPUT_ACTION_NONE);
}

static void test_decode_rxvt_csi_lowercase(void)
{
    /* rxvt lowercase CSI arrows denote Shift, which remains plain motion. */
    EXPECT(decode("[a") == INPUT_ACTION_HISTORY_PREV);
    EXPECT(decode("[b") == INPUT_ACTION_HISTORY_NEXT);
    EXPECT(decode("[c") == INPUT_ACTION_MOVE_RIGHT);
    EXPECT(decode("[d") == INPUT_ACTION_MOVE_LEFT);
}

static void test_decode_rxvt_ss3_lowercase(void)
{
    /* rxvt lowercase SS3 arrows denote Ctrl. */
    EXPECT(decode("Oa") == INPUT_ACTION_HISTORY_PREV);
    EXPECT(decode("Ob") == INPUT_ACTION_HISTORY_NEXT);
    EXPECT(decode("Oc") == INPUT_ACTION_MOVE_WORD_RIGHT);
    EXPECT(decode("Od") == INPUT_ACTION_MOVE_WORD_LEFT);
}

static void test_decode_rxvt_tilde_finals(void)
{
    /* rxvt encodes Ctrl/Shift in ^/$/@ finals; '$' is below ECMA-48's final-byte range. */
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
    /* iTerm2 Esc+ mode adds a second ESC before Option cursor keys. */
    EXPECT(decode("\x1b[D") == INPUT_ACTION_MOVE_WORD_LEFT);
    EXPECT(decode("\x1b[C") == INPUT_ACTION_MOVE_WORD_RIGHT);
    EXPECT(decode("\x1b[A") == INPUT_ACTION_HISTORY_PREV);

    EXPECT(decode("\x1b[1;5D") == INPUT_ACTION_MOVE_WORD_LEFT);

    EXPECT(decode("\x1b\x1b\x1b[D") == INPUT_ACTION_MOVE_WORD_LEFT);

    EXPECT(decode("\x1b\x1b\x1b\x1b\x1b[D") == INPUT_ACTION_NONE);
}

static void test_decode_meta_letters(void)
{
    EXPECT(decode("b") == INPUT_ACTION_MOVE_WORD_LEFT);
    EXPECT(decode("B") == INPUT_ACTION_MOVE_WORD_LEFT);
    EXPECT(decode("f") == INPUT_ACTION_MOVE_WORD_RIGHT);
    EXPECT(decode("F") == INPUT_ACTION_MOVE_WORD_RIGHT);
    EXPECT(decode("d") == INPUT_ACTION_KILL_WORD_FWD);
    EXPECT(decode("D") == INPUT_ACTION_KILL_WORD_FWD);

    /* Use explicit lengths for Alt+Backspace encodings containing control bytes. */
    unsigned char del = 0x7f;
    unsigned char bs = 0x08;
    EXPECT(decode_bytes(&del, 1) == INPUT_ACTION_KILL_WORD_BACK_ALNUM);
    EXPECT(decode_bytes(&bs, 1) == INPUT_ACTION_KILL_WORD_BACK_ALNUM);

    EXPECT(decode("\r") == INPUT_ACTION_INSERT_NEWLINE);
    EXPECT(decode("\n") == INPUT_ACTION_INSERT_NEWLINE);
}

static void test_decode_paste_begin(void)
{
    EXPECT(decode("[200~") == INPUT_ACTION_PASTE_BEGIN);
}

static void test_decode_partial_and_unknown(void)
{
    EXPECT(decode("") == INPUT_ACTION_NONE);

    EXPECT(decode("[1;5") == INPUT_ACTION_NONE);

    EXPECT(decode("[Z") == INPUT_ACTION_NONE);

    EXPECT(decode("OP") == INPUT_ACTION_NONE); /* F1 */
}

static void test_decode_overflow_and_runaway(void)
{
    /* Overflow must still consume through the final byte so the tail cannot leak as text. */
    unsigned char overflow_sequence[42];
    overflow_sequence[0] = '[';
    for (int i = 1; i < 41; i++)
        overflow_sequence[i] = '5';
    overflow_sequence[41] = 'D';
    struct byte_array_reader reader = {
        .bytes = overflow_sequence,
        .position = 0,
        .len = sizeof(overflow_sequence),
    };
    EXPECT(input_core_decode_escape(read_byte_array, &reader) == INPUT_ACTION_NONE);
    EXPECT(reader.position == sizeof(overflow_sequence));

    /* A missing final byte stops at the read cap instead of consuming indefinitely. */
    unsigned char runaway_sequence[100];
    runaway_sequence[0] = '[';
    for (size_t i = 1; i < sizeof(runaway_sequence); i++)
        runaway_sequence[i] = '5';
    EXPECT(decode_bytes(runaway_sequence, sizeof(runaway_sequence)) == INPUT_ACTION_NONE);
}

int main(void)
{
    /* mbrtowc needs a UTF-8 locale to exercise multi-byte layout. */
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
    test_render_combining_after_full_pending_buffer();

    test_window_top_fits();
    test_window_top_slides();
    test_render_window_hard_lines();
    test_render_window_from_top();
    test_render_window_soft_wrap_rows();

    test_set_buffer_and_insert();
    test_replace_span();
    test_commit_paste();
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
    test_history_search();
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
