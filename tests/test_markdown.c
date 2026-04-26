/* SPDX-License-Identifier: MIT */
#include "markdown.h"

#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "util.h"

/* Capture md_renderer output into a buf so tests can compare bytes. The
 * is_raw flag distinguishes ANSI escapes from content for downstream
 * routing in production; tests don't care, so we collapse both into one
 * stream of bytes and verify the literal byte sequence. */
static void capture(const char *bytes, size_t n, int is_raw, void *user)
{
    (void)is_raw;
    buf_append((struct buf *)user, bytes, n);
}

/* Common helper: feed the whole input as one block, flush, return string.
 * buf_steal returns NULL if nothing was appended; coerce to "" so callers
 * can EXPECT_STR_EQ unconditionally. */
static char *render_one(const char *input)
{
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out);
    md_feed(m, input, strlen(input));
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    return s ? s : xstrdup("");
}

/* ---------- pass-through ---------- */

static void test_plain_ascii(void)
{
    char *got = render_one("hello world");
    EXPECT_STR_EQ(got, "hello world");
    free(got);
}

static void test_utf8_passthrough(void)
{
    /* "héllo €" — multibyte chars must survive untouched. */
    const char *in = "h\xC3\xA9llo \xE2\x82\xAC";
    char *got = render_one(in);
    EXPECT_STR_EQ(got, in);
    free(got);
}

static void test_empty(void)
{
    char *got = render_one("");
    EXPECT_STR_EQ(got, "");
    free(got);
}

/* ---------- bold ---------- */

static void test_bold_simple(void)
{
    char *got = render_one("**hi**");
    EXPECT_STR_EQ(got, "\x1b[1mhi\x1b[22m");
    free(got);
}

static void test_bold_in_sentence(void)
{
    char *got = render_one("a **b** c");
    EXPECT_STR_EQ(got, "a \x1b[1mb\x1b[22m c");
    free(got);
}

/* ---------- italic ---------- */

static void test_italic_star(void)
{
    char *got = render_one("a *b* c");
    EXPECT_STR_EQ(got, "a \x1b[3mb\x1b[23m c");
    free(got);
}

static void test_italic_underscore(void)
{
    char *got = render_one("_b_");
    EXPECT_STR_EQ(got, "\x1b[3mb\x1b[23m");
    free(got);
}

/* ---------- inline code ---------- */

static void test_inline_code(void)
{
    char *got = render_one("a `read` b");
    EXPECT_STR_EQ(got, "a \x1b[36mread\x1b[39m b");
    free(got);
}

/* ---------- the weird case: bold-around-code ---------- */

static void test_bold_around_code(void)
{
    /* **`read`** must produce bold-on, cyan-on, "read", cyan-off, bold-off.
     * The verbatim-cyan "read" comes through because inline-code spans
     * don't process inner markers. */
    char *got = render_one("**`read`**");
    EXPECT_STR_EQ(got, "\x1b[1m\x1b[36mread\x1b[39m\x1b[22m");
    free(got);
}

/* ---------- headings ---------- */

static void test_heading_h3(void)
{
    char *got = render_one("### What it does\n");
    EXPECT_STR_EQ(got, "\x1b[1mWhat it does\x1b[22m\n");
    free(got);
}

static void test_heading_then_paragraph(void)
{
    char *got = render_one("## Tech\nDetails");
    EXPECT_STR_EQ(got, "\x1b[1mTech\x1b[22m\nDetails");
    free(got);
}

static void test_hash_no_space_is_text(void)
{
    /* `#define` is not a heading — needs a space after the hashes. */
    char *got = render_one("#define X");
    EXPECT_STR_EQ(got, "#define X");
    free(got);
}

/* ---------- code fence ---------- */

static void test_code_fence(void)
{
    /* Marker lines are consumed entirely (including their \n) so the dim
     * region wraps only the content lines. */
    char *got = render_one("```\nfoo\nbar\n```\n");
    EXPECT_STR_EQ(got, "\x1b[2mfoo\nbar\n\x1b[22m");
    free(got);
}

static void test_code_fence_with_lang(void)
{
    /* Language id renders as a dim+cyan label on its own line above
     * the content; dim is already on from the fence opener, so cyan
     * layers on it and the body returns to dim+default fg. */
    char *got = render_one("```c\nint x;\n```\n");
    EXPECT_STR_EQ(got, "\x1b[2m\x1b[36mc\x1b[39m\nint x;\n\x1b[22m");
    free(got);
}

static void test_code_fence_lang_with_attrs(void)
{
    /* Info string with attributes: only the first token is the label. */
    char *got = render_one("```python title=\"x.py\"\nprint(1)\n```\n");
    EXPECT_STR_EQ(got, "\x1b[2m\x1b[36mpython\x1b[39m\nprint(1)\n\x1b[22m");
    free(got);
}

static void test_code_fence_lang_split_across_feeds(void)
{
    /* Opener line spans feeds — the opener defers until we have \n,
     * so the language is captured cleanly on the second feed. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out);
    md_feed(m, "```py", 5);
    md_feed(m, "thon\nint x;\n```\n", 16);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "\x1b[2m\x1b[36mpython\x1b[39m\nint x;\n\x1b[22m");
    free(s);
}

static void test_code_fence_inner_markers_verbatim(void)
{
    /* `*` inside a fence must NOT toggle italic; the fence is verbatim. */
    char *got = render_one("```\n*not italic*\n```\n");
    EXPECT_STR_EQ(got, "\x1b[2m*not italic*\n\x1b[22m");
    free(got);
}

static void test_code_fence_inner_with_info_is_content(void)
{
    /* Inside a 3-backtick fence, a ```python line has an info string —
     * per CommonMark it can't be a closer, so it stays as content. The
     * outer fence remains open until the bare ``` line. This is the
     * scenario from the markdown-rendering-demo trace. */
    const char *in = "```markdown\n# Title\n```python\ndef f(): pass\n```\n";
    char *got = render_one(in);
    const char *want = "\x1b[2m\x1b[36mmarkdown\x1b[39m\n"
                       "# Title\n```python\ndef f(): pass\n\x1b[22m";
    EXPECT_STR_EQ(got, want);
    free(got);
}

static void test_code_fence_four_backticks_wraps_three(void)
{
    /* A 4-backtick fence stays open across inner ``` lines — common
     * pattern for documenting code that itself contains backticks. */
    const char *in = "````\nthis has ```inner``` text\n````\n";
    char *got = render_one(in);
    const char *want = "\x1b[2mthis has ```inner``` text\n\x1b[22m";
    EXPECT_STR_EQ(got, want);
    free(got);
}

static void test_code_fence_three_backticks_dont_close_four(void)
{
    /* A 3-backtick line inside a 4-backtick fence stays as content. */
    const char *in = "````\nbody\n```\nstill body\n````\n";
    char *got = render_one(in);
    const char *want = "\x1b[2mbody\n```\nstill body\n\x1b[22m";
    EXPECT_STR_EQ(got, want);
    free(got);
}

static void test_code_fence_closer_with_trailing_spaces(void)
{
    /* CommonMark allows trailing whitespace on the closer line. */
    char *got = render_one("```\nfoo\n```   \n");
    EXPECT_STR_EQ(got, "\x1b[2mfoo\n\x1b[22m");
    free(got);
}

static void test_code_fence_crlf_closer(void)
{
    /* CRLF line endings — the trailing \r before \n must be treated as
     * whitespace, otherwise the closer is misread as content. */
    char *got = render_one("```\r\nfoo\r\n```\r\n");
    EXPECT_STR_EQ(got, "\x1b[2mfoo\r\n\x1b[22m");
    free(got);
}

static void test_code_fence_crlf_lang(void)
{
    /* CRLF on the opener line — \r must not leak into the language
     * label. */
    char *got = render_one("```python\r\nx = 1\r\n```\r\n");
    EXPECT_STR_EQ(got, "\x1b[2m\x1b[36mpython\x1b[39m\nx = 1\r\n\x1b[22m");
    free(got);
}

/* ---------- list markers ---------- */

static void test_star_space_is_list_marker(void)
{
    /* `* foo` at line start renders the `*` literally — must not enter italic. */
    char *got = render_one("* foo");
    EXPECT_STR_EQ(got, "* foo");
    free(got);
}

static void test_dash_list_passthrough(void)
{
    /* `- foo` has no special meaning to our parser; it just renders. */
    char *got = render_one("- foo");
    EXPECT_STR_EQ(got, "- foo");
    free(got);
}

static void test_numbered_list_passthrough(void)
{
    char *got = render_one("1. foo\n2. bar");
    EXPECT_STR_EQ(got, "1. foo\n2. bar");
    free(got);
}

static void test_list_with_bold(void)
{
    /* Snippet pattern: `- **bold**: rest` */
    char *got = render_one("- **bold**: rest");
    EXPECT_STR_EQ(got, "- \x1b[1mbold\x1b[22m: rest");
    free(got);
}

/* ---------- streaming: marker split across feeds ---------- */

static void test_split_double_star(void)
{
    /* `**` arrives as `*` then `*` — must produce a single bold toggle. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out);
    md_feed(m, "*", 1);
    md_feed(m, "*hi*", 4);
    md_feed(m, "*", 1);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "\x1b[1mhi\x1b[22m");
    free(s);
}

static void test_split_inline_code(void)
{
    /* `` ` `` arrives at end of one feed; check it doesn't fence-mistake. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out);
    md_feed(m, "x ", 2); /* clear at_line_start */
    md_feed(m, "`", 1);
    md_feed(m, "y`", 2);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "x \x1b[36my\x1b[39m");
    free(s);
}

static void test_split_fence_at_line_start(void)
{
    /* Fence open arrives across three feeds, all at line start. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out);
    md_feed(m, "`", 1);
    md_feed(m, "`", 1);
    md_feed(m, "`\nbody\n```\n", 11);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "\x1b[2mbody\n\x1b[22m");
    free(s);
}

static void test_split_heading(void)
{
    /* `### ` arrives across feeds. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out);
    md_feed(m, "##", 2);
    md_feed(m, "# title\n", 8);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "\x1b[1mtitle\x1b[22m\n");
    free(s);
}

/* ---------- flush behavior ---------- */

static void test_flush_emits_unterminated_marker(void)
{
    /* A `**bold` with no closing — flush should emit the bold-on but
     * close it cleanly so the terminal isn't left in a styled state. */
    char *got = render_one("**bold");
    EXPECT_STR_EQ(got, "\x1b[1mbold\x1b[22m");
    free(got);
}

static void test_flush_emits_pending_tail(void)
{
    /* A trailing `*` that never gets a partner — emit literally on flush. */
    char *got = render_one("foo*");
    EXPECT_STR_EQ(got, "foo*");
    free(got);
}

static void test_flush_closes_italic_at_eof(void)
{
    /* `*hi*` ends without a trailing byte; the closing `*` defers because
     * the streaming loop can't tell if a `**` would follow. md_flush must
     * recognize that we're in_italic and treat the tail `*` as the closer. */
    char *got = render_one("*hi*");
    EXPECT_STR_EQ(got, "\x1b[3mhi\x1b[23m");
    free(got);
}

static void test_flush_closes_underscore_italic_at_eof(void)
{
    char *got = render_one("_hi_");
    EXPECT_STR_EQ(got, "\x1b[3mhi\x1b[23m");
    free(got);
}

static void test_flush_closes_bold_at_eof(void)
{
    /* `**hi**` with no trailing byte after the second `**` — the closer
     * defers and md_flush must recognize the bold close. */
    char *got = render_one("**hi**");
    EXPECT_STR_EQ(got, "\x1b[1mhi\x1b[22m");
    free(got);
}

static void test_flush_closes_fence_at_eof(void)
{
    /* Fenced block with no trailing newline after the closing ``` — the
     * backticks defer (couldn't see whether more would follow) and
     * md_flush must recognize them as a valid closer. */
    char *got = render_one("```\nfoo\n```");
    EXPECT_STR_EQ(got, "\x1b[2mfoo\n\x1b[22m");
    free(got);
}

/* ---------- reset ---------- */

static void test_reset_clears_state(void)
{
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out);
    md_feed(m, "**still bold", 12);
    md_reset(m);
    buf_reset(&out);
    md_feed(m, "plain", 5);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    /* No bold leakage from the previous turn. */
    EXPECT_STR_EQ(s, "plain");
    free(s);
}

/* ---------- markers inside code spans/fences must stay verbatim ---------- */

static void test_inline_code_bold_marker_verbatim(void)
{
    /* `**foo**` — bold-marker bytes inside backticks must not toggle bold. */
    char *got = render_one("see `**foo**` here");
    EXPECT_STR_EQ(got, "see \x1b[36m**foo**\x1b[39m here");
    free(got);
}

static void test_inline_code_underscore_verbatim(void)
{
    /* The user's reported case wrapped in inline code. */
    char *got = render_one("`compile_commands.json`");
    EXPECT_STR_EQ(got, "\x1b[36mcompile_commands.json\x1b[39m");
    free(got);
}

static void test_code_fence_all_markers_verbatim(void)
{
    char *got = render_one("```\n**bold** and _italic_ and `code`\n```\n");
    EXPECT_STR_EQ(got, "\x1b[2m**bold** and _italic_ and `code`\n\x1b[22m");
    free(got);
}

/* ---------- intraword markers must stay literal ---------- */

static void test_intraword_underscore_literal(void)
{
    /* Identifiers and paths with underscores must not turn italic at the
     * first `_`. This was the user-reported regression. */
    char *got = render_one("compile_commands.json symlink");
    EXPECT_STR_EQ(got, "compile_commands.json symlink");
    free(got);
}

static void test_intraword_star_literal(void)
{
    /* `5*3*7` — `*` between digits must not toggle italic. */
    char *got = render_one("5*3*7");
    EXPECT_STR_EQ(got, "5*3*7");
    free(got);
}

static void test_intraword_double_star_literal(void)
{
    /* `compile**name**rest` — `**` with alpha on the left can't open. */
    char *got = render_one("compile**name**rest");
    EXPECT_STR_EQ(got, "compile**name**rest");
    free(got);
}

static void test_underscore_at_word_boundary_opens(void)
{
    /* `_word_` — left side is start-of-stream, right side is alpha; the
     * opening rule (left non-alpha) is satisfied. Closing happens
     * unconditionally because we're already in italic. */
    char *got = render_one("_word_");
    EXPECT_STR_EQ(got, "\x1b[3mword\x1b[23m");
    free(got);
}

static void test_intraword_underscore_split_across_feeds(void)
{
    /* prev_byte from the previous feed must be remembered so that the `_`
     * starting the next feed is correctly seen as intraword. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out);
    md_feed(m, "compile", 7);
    md_feed(m, "_commands", 9);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "compile_commands");
    free(s);
}

static void test_underscore_after_space_split_across_feeds(void)
{
    /* prev_byte = ' ' carried across feeds — `_` at start of next feed
     * should open italic. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out);
    md_feed(m, "say ", 4);
    md_feed(m, "_yes_", 5);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "say \x1b[3myes\x1b[23m");
    free(s);
}

/* ---------- emphasis must not open when right side is whitespace ---------- */

static void test_spaced_star_arithmetic(void)
{
    /* `5 * 3 * 7` — `*` flanked by spaces can't open italic. */
    char *got = render_one("5 * 3 * 7");
    EXPECT_STR_EQ(got, "5 * 3 * 7");
    free(got);
}

static void test_spaced_double_star(void)
{
    /* `5 ** 3` — `**` flanked by spaces can't open bold. */
    char *got = render_one("5 ** 3");
    EXPECT_STR_EQ(got, "5 ** 3");
    free(got);
}

static void test_indented_list_marker_literal(void)
{
    /* Indented `  * item` — line-start path doesn't catch this because
     * leading whitespace clears at_line_start, so the `*` falls through
     * to inline. The right-side-whitespace check must keep it literal. */
    char *got = render_one("  * item");
    EXPECT_STR_EQ(got, "  * item");
    free(got);
}

static void test_underscore_with_space_right(void)
{
    /* `_ word` — `_` followed by space can't open italic. */
    char *got = render_one("a _ b");
    EXPECT_STR_EQ(got, "a _ b");
    free(got);
}

/* ---------- long fence opener split across deltas ---------- */

static void test_fence_long_info_split_across_feeds(void)
{
    /* Opener line longer than the old TAIL_CAP=32, split mid-info. The
     * deferred opener must be buffered intact so the fence opens cleanly
     * once the \n arrives — not flushed as plain text, which would let
     * the closing ``` start a new empty fence. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out);
    md_feed(m, "```python title=\"some/very/long/", 32);
    md_feed(m, "path/here.py\"\nx = 1\n```\n", 25);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "\x1b[2m\x1b[36mpython\x1b[39m\nx = 1\n\x1b[22m");
    free(s);
}

/* ---------- mixed snippet from real session ---------- */

static void test_real_world_paragraph(void)
{
    /* Compact slice that exercises bold, inline code, em dash, plain text. */
    const char *in = "I'm **hax**, with `read` and `bash` tools.";
    char *got = render_one(in);
    const char *want = "I'm \x1b[1mhax\x1b[22m, with \x1b[36mread\x1b[39m and "
                       "\x1b[36mbash\x1b[39m tools.";
    EXPECT_STR_EQ(got, want);
    free(got);
}

int main(void)
{
    test_plain_ascii();
    test_utf8_passthrough();
    test_empty();

    test_bold_simple();
    test_bold_in_sentence();

    test_italic_star();
    test_italic_underscore();

    test_inline_code();
    test_bold_around_code();

    test_heading_h3();
    test_heading_then_paragraph();
    test_hash_no_space_is_text();

    test_code_fence();
    test_code_fence_with_lang();
    test_code_fence_lang_with_attrs();
    test_code_fence_lang_split_across_feeds();
    test_code_fence_inner_markers_verbatim();
    test_code_fence_inner_with_info_is_content();
    test_code_fence_four_backticks_wraps_three();
    test_code_fence_three_backticks_dont_close_four();
    test_code_fence_closer_with_trailing_spaces();
    test_code_fence_crlf_closer();
    test_code_fence_crlf_lang();

    test_star_space_is_list_marker();
    test_dash_list_passthrough();
    test_numbered_list_passthrough();
    test_list_with_bold();

    test_split_double_star();
    test_split_inline_code();
    test_split_fence_at_line_start();
    test_split_heading();

    test_flush_emits_unterminated_marker();
    test_flush_emits_pending_tail();
    test_flush_closes_italic_at_eof();
    test_flush_closes_underscore_italic_at_eof();
    test_flush_closes_bold_at_eof();
    test_flush_closes_fence_at_eof();

    test_reset_clears_state();

    test_inline_code_bold_marker_verbatim();
    test_inline_code_underscore_verbatim();
    test_code_fence_all_markers_verbatim();

    test_intraword_underscore_literal();
    test_intraword_star_literal();
    test_intraword_double_star_literal();
    test_underscore_at_word_boundary_opens();
    test_intraword_underscore_split_across_feeds();
    test_underscore_after_space_split_across_feeds();

    test_spaced_star_arithmetic();
    test_spaced_double_star();
    test_indented_list_marker_literal();
    test_underscore_with_space_right();

    test_fence_long_info_split_across_feeds();

    test_real_world_paragraph();

    T_REPORT();
}
