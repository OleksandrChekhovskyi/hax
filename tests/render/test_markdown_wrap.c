/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "render/markdown_wrap.h"
#include "terminal/ansi.h"

#define BLD    ANSI_BOLD
#define OFF    ANSI_BOLD_OFF
#define ERASE  ANSI_ERASE_LINE
#define CUB(n) "\x1b[" #n "D"

struct capture {
    struct buf text;
    struct buf raw;
    struct buf wire;
    int calls;
};

static void capture_emit(const char *bytes, size_t n, int is_raw, void *user)
{
    struct capture *c = user;
    buf_append(is_raw ? &c->raw : &c->text, bytes, n);
    buf_append(&c->wire, bytes, n);
    c->calls++;
}

static const char *buf_string(const struct buf *b)
{
    return b->data ? b->data : "";
}

static void setup(struct md_wrap *w, struct md_wrap_context *ctx, struct capture *c, int width)
{
    memset(w, 0, sizeof(*w));
    memset(c, 0, sizeof(*c));
    md_wrap_reset(w, width);
    *ctx = (struct md_wrap_context){
        .emit = capture_emit,
        .user = c,
        .styled = 1,
    };
}

static void capture_reset(struct capture *c)
{
    buf_reset(&c->text);
    buf_reset(&c->raw);
    buf_reset(&c->wire);
    c->calls = 0;
}

static void teardown(struct md_wrap *w, struct capture *c)
{
    md_wrap_free(w);
    buf_free(&c->text);
    buf_free(&c->raw);
    buf_free(&c->wire);
}

static void emit_text(struct md_wrap *w, const struct md_wrap_context *ctx, const char *s)
{
    md_wrap_emit_text(w, ctx, s, strlen(s), 0);
}

static void emit_raw(struct md_wrap *w, const struct md_wrap_context *ctx, const char *s)
{
    md_wrap_emit_raw(w, ctx, s, strlen(s), 0);
}

static void test_text_emits_eagerly(void)
{
    struct md_wrap w;
    struct md_wrap_context ctx;
    struct capture c;
    setup(&w, &ctx, &c, 100);

    /* Streaming output must not wait for a later word or final flush. */
    emit_text(&w, &ctx, "alpha ");
    EXPECT_STR_EQ(buf_string(&c.wire), "alpha ");
    EXPECT(c.calls == 6);
    emit_text(&w, &ctx, "beta");
    EXPECT_STR_EQ(buf_string(&c.wire), "alpha beta");
    EXPECT(c.calls == 10);
    md_wrap_flush(&w, &ctx);
    EXPECT(c.calls == 10);

    teardown(&w, &c);
}

static void test_retroactive_break_wire_and_kinds(void)
{
    struct md_wrap w;
    struct md_wrap_context ctx;
    struct capture c;
    setup(&w, &ctx, &c, 10);

    /* Retro-wrap controls are raw; replayed text and the newline remain content. */
    emit_text(&w, &ctx, "abc def ghi");
    md_wrap_flush(&w, &ctx);

    EXPECT_STR_EQ(buf_string(&c.wire), "abc def gh" CUB(3) ERASE "\nghi");
    EXPECT_STR_EQ(buf_string(&c.text), "abc def gh\nghi");
    EXPECT_STR_EQ(buf_string(&c.raw), CUB(3) ERASE);

    teardown(&w, &c);
}

static void test_tab_policies(void)
{
    struct md_wrap w;
    struct md_wrap_context ctx;
    struct capture c;
    setup(&w, &ctx, &c, 4);

    /* Wrapped tabs collapse to one break-space; verbatim tabs preserve four. */
    emit_text(&w, &ctx, "ab\tcd");
    md_wrap_flush(&w, &ctx);
    EXPECT_STR_EQ(buf_string(&c.wire), "ab c" CUB(2) ERASE "\ncd");

    capture_reset(&c);
    md_wrap_reset(&w, 4);
    md_wrap_emit_text(&w, &ctx, "ab\tcd", 5, 1);
    EXPECT_STR_EQ(buf_string(&c.wire), "ab    cd");

    capture_reset(&c);
    md_wrap_reset(&w, 0);
    emit_text(&w, &ctx, "ab\tcd");
    EXPECT_STR_EQ(buf_string(&c.wire), "ab    cd");

    teardown(&w, &c);
}

static void test_partial_utf8_drains_before_boundaries(void)
{
    struct md_wrap w;
    struct md_wrap_context ctx;
    struct capture c;
    setup(&w, &ctx, &c, 80);

    /* An incomplete codepoint must stay before the newline or raw run that follows. */
    md_wrap_emit_text(&w, &ctx, "A\xC3", 2, 0);
    emit_text(&w, &ctx, "\nX");
    EXPECT_STR_EQ(buf_string(&c.wire), "A\xC3\nX");

    capture_reset(&c);
    md_wrap_reset(&w, 80);
    md_wrap_emit_text(&w, &ctx, "A\xC3", 2, 0);
    emit_raw(&w, &ctx, BLD);
    EXPECT_STR_EQ(buf_string(&c.wire), "A\xC3" BLD);
    EXPECT_STR_EQ(buf_string(&c.text), "A\xC3");
    EXPECT_STR_EQ(buf_string(&c.raw), BLD);

    teardown(&w, &c);
}

static void test_split_multibyte_counts_cells(void)
{
    struct md_wrap w;
    struct md_wrap_context ctx;
    struct capture c;
    setup(&w, &ctx, &c, 8);

    /* Feed boundaries inside UTF-8 must not change cell counts or break placement. */
    md_wrap_emit_text(&w, &ctx, "h\xC3", 2, 0);
    const char *tail = "\xA9llo w\xC3\xB6rld";
    md_wrap_emit_text(&w, &ctx, tail, strlen(tail), 0);
    md_wrap_flush(&w, &ctx);

    EXPECT_STR_EQ(buf_string(&c.wire), "h\xC3\xA9llo w\xC3\xB6" CUB(3) ERASE "\nw\xC3\xB6rld");

    teardown(&w, &c);
}

static void test_pending_edge_wrap(void)
{
    struct md_wrap w;
    struct md_wrap_context ctx;
    struct capture c;
    setup(&w, &ctx, &c, 7);

    /* Edge-spaces stay off the wire; direct block output commits the deferred wrap. */
    emit_text(&w, &ctx, "foo bar  ");
    EXPECT_STR_EQ(buf_string(&c.wire), "foo bar");
    md_wrap_commit_pending(&w, &ctx);
    EXPECT_STR_EQ(buf_string(&c.wire), "foo bar\n");

    teardown(&w, &c);
}

static void test_hard_newline_absorbs_pending(void)
{
    struct md_wrap w;
    struct md_wrap_context ctx;
    struct capture c;
    setup(&w, &ctx, &c, 5);

    /* A source newline absorbs an edge wrap so CRLF cannot create a blank row. */
    emit_text(&w, &ctx, "aaaaa  \r\nbar");
    md_wrap_flush(&w, &ctx);
    EXPECT_STR_EQ(buf_string(&c.wire), "aaaaa\nbar");
    EXPECT_STR_EQ(buf_string(&c.raw), "");

    teardown(&w, &c);
}

static void test_raw_does_not_commit_pending(void)
{
    struct md_wrap w;
    struct md_wrap_context ctx;
    struct capture c;
    setup(&w, &ctx, &c, 7);

    /* Zero-width SGR must not strand a deferred newline on an empty row. */
    emit_text(&w, &ctx, "foo bar ");
    emit_raw(&w, &ctx, BLD);
    ctx.in_bold = 1;
    emit_text(&w, &ctx, "baz");
    emit_raw(&w, &ctx, OFF);
    ctx.in_bold = 0;
    md_wrap_flush(&w, &ctx);

    EXPECT_STR_EQ(buf_string(&c.wire), "foo bar" BLD "\nbaz" OFF);

    teardown(&w, &c);
}

static void test_list_marker_sets_hanging_indent(void)
{
    struct md_wrap w;
    struct md_wrap_context ctx;
    struct capture c;
    setup(&w, &ctx, &c, 15);

    /* Continuation rows align under list content rather than the marker. */
    emit_text(&w, &ctx, "* alpha beta gamma");
    md_wrap_flush(&w, &ctx);

    EXPECT_STR_EQ(buf_string(&c.wire), "* alpha beta ga" CUB(3) ERASE "\n  gamma");

    teardown(&w, &c);
}

static void test_replay_restores_style_snapshot(void)
{
    struct md_wrap w;
    struct md_wrap_context ctx;
    struct capture c;
    setup(&w, &ctx, &c, 8);

    /* Replay must restore style at the break, not the later eagerly emitted state. */
    emit_raw(&w, &ctx, BLD);
    ctx.in_bold = 1;
    emit_text(&w, &ctx, "foo bar");
    emit_raw(&w, &ctx, OFF);
    ctx.in_bold = 0;
    emit_text(&w, &ctx, "baz");
    md_wrap_flush(&w, &ctx);

    EXPECT_STR_EQ(buf_string(&c.wire), BLD "foo bar" OFF "b" CUB(5) ERASE "\n" BLD "bar" OFF "baz");

    teardown(&w, &c);
}

int main(void)
{
    locale_init_utf8();

    test_text_emits_eagerly();
    test_retroactive_break_wire_and_kinds();
    test_tab_policies();
    test_partial_utf8_drains_before_boundaries();
    test_split_multibyte_counts_cells();
    test_pending_edge_wrap();
    test_hard_newline_absorbs_pending();
    test_raw_does_not_commit_pending();
    test_list_marker_sets_hanging_indent();
    test_replay_restores_style_snapshot();

    T_REPORT();
}
