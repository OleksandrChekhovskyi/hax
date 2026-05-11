/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "util.h"
#include "render/markdown.h"

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
    struct md_renderer *m = md_new(capture, &out, 0);
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

static void test_code_fence_tab_expanded_to_four_spaces(void)
{
    /* Fence body bypasses the wrap engine; emit_text substitutes \t
     * with 4 spaces before either branch so terminals don't expand
     * tabs to inconsistent column-multiple-of-8 stops. */
    char *got = render_one("```\n\thello\n```\n");
    EXPECT(strchr(got, '\t') == NULL);
    EXPECT(strstr(got, "    hello") != NULL);
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
    struct md_renderer *m = md_new(capture, &out, 0);
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
    struct md_renderer *m = md_new(capture, &out, 0);
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
    struct md_renderer *m = md_new(capture, &out, 0);
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
    struct md_renderer *m = md_new(capture, &out, 0);
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
    struct md_renderer *m = md_new(capture, &out, 0);
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
    struct md_renderer *m = md_new(capture, &out, 0);
    md_feed(m, "**still bold", 12);
    md_reset(m, 0);
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
    struct md_renderer *m = md_new(capture, &out, 0);
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
    struct md_renderer *m = md_new(capture, &out, 0);
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
    struct md_renderer *m = md_new(capture, &out, 0);
    md_feed(m, "```python title=\"some/very/long/", 32);
    md_feed(m, "path/here.py\"\nx = 1\n```\n", 25);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "\x1b[2m\x1b[36mpython\x1b[39m\nx = 1\n\x1b[22m");
    free(s);
}

/* ---------- soft-break joining ---------- */

static void test_soft_join_user_review_pattern(void)
{
    /* The exact pattern from the user's transcript: a list item
     * whose content ends in `.**` (period, then closing bold) and
     * is followed by an indented continuation. Soft-join should
     * yield exactly ONE space between the closing `**` and the
     * continuation text, not two. */
    char *got =
        render_one("- **P3 `src/path:line` - lorem ipsum.**\n  The detailed explanation follows.");
    EXPECT_STR_EQ(got, "- \x1b[1mP3 \x1b[36msrc/path:line\x1b[39m - lorem ipsum.\x1b[22m"
                       " The detailed explanation follows.");
    free(got);
}

static void test_eof_thematic_no_trailing_newline(void)
{
    /* Thematic break at end of stream without a trailing \n. The
     * streaming \n handler defers waiting for the line terminator,
     * but at md_flush we know the bytes-we-have ARE the whole line,
     * and a 3+ marker run is a valid thematic break. */
    char *got = render_one("section\n---");
    EXPECT_STR_EQ(got, "section\n---");
    free(got);
}

static void test_eof_setext_h1_no_trailing_newline(void)
{
    /* Same for setext h1 underline `=====`. */
    char *got = render_one("Heading\n=====");
    EXPECT_STR_EQ(got, "Heading\n=====");
    free(got);
}

static void test_eof_setext_h2_no_trailing_newline(void)
{
    /* Setext h2 underline `-----`. */
    char *got = render_one("Heading\n-----");
    EXPECT_STR_EQ(got, "Heading\n-----");
    free(got);
}

static void test_eof_thematic_spaced_no_trailing_newline(void)
{
    char *got = render_one("section\n_ _ _");
    EXPECT_STR_EQ(got, "section\n_ _ _");
    free(got);
}

static void test_crlf_thematic_break(void)
{
    /* CRLF line endings — \r before \n in the marker line must be
     * treated as whitespace by the soft-break thematic scan, so
     * the marker is recognized. */
    char *got = render_one("section\n---\r\nnext");
    EXPECT_STR_EQ(got, "section\n---\r\nnext");
    free(got);
}

static void test_eof_soft_join_digits(void)
{
    /* "foo\n2024" at end-of-stream: streaming `\n` handler defers
     * because `2024` could be the start of `2024. ` numbered list.
     * At md_flush no more bytes will arrive, so resolve the defer
     * as soft-join — not the literal `foo\n2024`. */
    char *got = render_one("foo\n2024");
    EXPECT_STR_EQ(got, "foo 2024");
    free(got);
}

static void test_eof_soft_join_dash_alone(void)
{
    /* "foo\n-" at EOF — could have been `- item` but didn't. */
    char *got = render_one("foo\n-");
    EXPECT_STR_EQ(got, "foo -");
    free(got);
}

static void test_eof_soft_join_hash_alone(void)
{
    /* "foo\n#" at EOF — could have been heading but no space follows. */
    char *got = render_one("foo\n#");
    EXPECT_STR_EQ(got, "foo #");
    free(got);
}

static void test_eof_hard_break_blank(void)
{
    /* "foo\n   " at EOF (just whitespace after \n) — emit \n
     * (paragraph terminator); the trailing whitespace is dropped. */
    char *got = render_one("foo\n   ");
    EXPECT_STR_EQ(got, "foo\n");
    free(got);
}

static void test_soft_break_simple_join(void)
{
    /* Single \n inside a paragraph is treated as a space (CommonMark). */
    char *got = render_one("foo\nbar");
    EXPECT_STR_EQ(got, "foo bar");
    free(got);
}

static void test_soft_break_skip_leading_whitespace(void)
{
    /* Leading whitespace on the joined line is absorbed so we don't
     * double-space. */
    char *got = render_one("foo\n   bar");
    EXPECT_STR_EQ(got, "foo bar");
    free(got);
}

static void test_soft_break_no_double_space(void)
{
    /* Trailing space on the source line + \n must not produce two
     * spaces in the joined output. */
    char *got = render_one("foo \nbar");
    EXPECT_STR_EQ(got, "foo bar");
    free(got);
}

static void test_hard_break_blank_line(void)
{
    /* "\n\n" is a paragraph break — both \n's preserved. */
    char *got = render_one("foo\n\nbar");
    EXPECT_STR_EQ(got, "foo\n\nbar");
    free(got);
}

static void test_hard_break_blank_line_4_spaces(void)
{
    /* Whitespace-only line with 4+ spaces (past the 3-space marker
     * cap) is still a blank line — paragraph break preserved, not
     * collapsed via soft-join. */
    char *got = render_one("foo\n    \nbar");
    EXPECT_STR_EQ(got, "foo\n\nbar");
    free(got);
}

static void test_hard_break_blank_line_with_tab(void)
{
    /* Whitespace-only line with a tab — same. */
    char *got = render_one("foo\n\t\nbar");
    EXPECT_STR_EQ(got, "foo\n\nbar");
    free(got);
}

static void test_soft_join_hash_no_space(void)
{
    /* `##not heading` is not a CommonMark heading (no space after
     * the hash run). Soft-join the line into the preceding prose
     * instead of inserting a stray hard break. */
    char *got = render_one("foo\n##not heading");
    EXPECT_STR_EQ(got, "foo ##not heading");
    free(got);
}

static void test_line_start_hash_no_space(void)
{
    /* Same at actual line start with leading spaces — `  ##not`
     * doesn't normalize the leading spaces because it isn't a
     * heading marker. Falls through to plain text. */
    char *got = render_one("  ##not heading");
    EXPECT_STR_EQ(got, "  ##not heading");
    free(got);
}

static void test_hard_break_list_marker(void)
{
    /* Next line starting with a list marker is a hard break. */
    char *got = render_one("foo\n* item");
    EXPECT_STR_EQ(got, "foo\n* item");
    free(got);
}

static void test_hard_break_dash_list(void)
{
    char *got = render_one("foo\n- item");
    EXPECT_STR_EQ(got, "foo\n- item");
    free(got);
}

static void test_hard_break_numbered_list(void)
{
    char *got = render_one("foo\n1. item");
    EXPECT_STR_EQ(got, "foo\n1. item");
    free(got);
}

static void test_hard_break_heading(void)
{
    /* Next line starting with a heading marker is hard. */
    char *got = render_one("foo\n## h\n");
    EXPECT_STR_EQ(got, "foo\n\x1b[1mh\x1b[22m\n");
    free(got);
}

static void test_hard_break_fence(void)
{
    /* Next line starting with ``` opens a fence — must be hard. */
    char *got = render_one("foo\n```\ncode\n```\n");
    EXPECT_STR_EQ(got, "foo\n\x1b[2mcode\n\x1b[22m");
    free(got);
}

static void test_soft_break_carries_bold(void)
{
    /* Bold style continues across a soft break — we don't close on
     * soft \n the way we do on hard. */
    char *got = render_one("**foo\nbar**");
    EXPECT_STR_EQ(got, "\x1b[1mfoo bar\x1b[22m");
    free(got);
}

static void test_soft_break_across_feeds(void)
{
    /* The \n at the boundary defers (couldn't see next line's first
     * byte) and is decided on the second feed. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 0);
    md_feed(m, "foo\n", 4);
    md_feed(m, "bar", 3);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "foo bar");
    free(s);
}

static void test_soft_break_paragraph_break_across_feeds(void)
{
    /* "\n" followed later by "\nbar" — the deferred \n combines with
     * the next feed's leading \n into a hard paragraph break. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 0);
    md_feed(m, "foo\n", 4);
    md_feed(m, "\nbar", 4);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "foo\n\nbar");
    free(s);
}

static void test_hard_break_indented_bullet_preserves_indent(void)
{
    /* Up to 3 leading spaces before a list marker is still a list
     * (CommonMark). The leading spaces are PRESERVED so a nested
     * list keeps its visual hierarchy — `* parent\n  - child`
     * stays nested rather than being flattened to top level. */
    char *got = render_one("* parent\n  - child\n  - sibling");
    EXPECT_STR_EQ(got, "* parent\n  - child\n  - sibling");
    free(got);
}

static void test_hard_break_indented_bullet_no_parent(void)
{
    /* Same shape without a containing list — the indent is
     * preserved (we can't tell from the byte stream whether it's
     * top-level allowance or nesting). Renders the leading spaces
     * literally; CommonMark would normalize but our pragmatic
     * approach errs on the side of preserving model intent. */
    char *got = render_one("intro\n  - one\n  - two");
    EXPECT_STR_EQ(got, "intro\n  - one\n  - two");
    free(got);
}

static void test_hard_break_indented_heading(void)
{
    /* `  ## sub` is still a heading. The leading spaces are dropped
     * (CommonMark equivalence) so the heading renders as bold. */
    char *got = render_one("preamble\n  ## sub\nbody");
    EXPECT_STR_EQ(got, "preamble\n\x1b[1msub\x1b[22m\nbody");
    free(got);
}

static void test_hard_break_indented_fence(void)
{
    /* Indented fence opener — same equivalence. */
    char *got = render_one("intro\n  ```\ncode\n```\n");
    EXPECT_STR_EQ(got, "intro\n\x1b[2mcode\n\x1b[22m");
    free(got);
}

static void test_soft_join_4_leading_spaces_not_marker(void)
{
    /* 4+ leading spaces falls outside the CommonMark "indented block
     * marker" allowance — it's an indented code block in CommonMark,
     * but we don't support that. Soft-join eats the whitespace. */
    char *got = render_one("intro\n    - looks like marker");
    EXPECT_STR_EQ(got, "intro - looks like marker");
    free(got);
}

static void test_line_start_indented_heading(void)
{
    /* Up to 3 leading spaces before a heading at the very start of
     * the input (or after a blank line) is equivalent to no indent
     * per CommonMark. step_line_start must normalize, not just the
     * soft-break path. */
    char *got = render_one("  ## h\nbody");
    EXPECT_STR_EQ(got, "\x1b[1mh\x1b[22m\nbody");
    free(got);
}

static void test_line_start_indented_fence(void)
{
    char *got = render_one("  ```\ncode\n```\n");
    EXPECT_STR_EQ(got, "\x1b[2mcode\n\x1b[22m");
    free(got);
}

static void test_line_start_indented_thematic(void)
{
    /* Thematic breaks are CommonMark-equivalent under leading
     * spaces — the marker line is consumed verbatim from the
     * normalized position, so the leading spaces don't survive. */
    char *got = render_one("  ---\nbody");
    EXPECT_STR_EQ(got, "---\nbody");
    free(got);
}

static void test_line_start_indented_after_blank(void)
{
    /* `intro\n\n  ```\n…` — fence after blank line at line start
     * with leading spaces. Same path. */
    char *got = render_one("intro\n\n  ```\ncode\n```\n");
    EXPECT_STR_EQ(got, "intro\n\n\x1b[2mcode\n\x1b[22m");
    free(got);
}

static void test_line_start_indented_bullet_preserves(void)
{
    /* Bullets are NOT normalized — leading spaces stay so nested
     * lists keep their visual indent even when arriving at line
     * start. */
    char *got = render_one("  - item");
    EXPECT_STR_EQ(got, "  - item");
    free(got);
}

static void test_hard_break_blockquote(void)
{
    /* `>` at line start is a blockquote marker — must keep rows
     * separate even though we don't pretty-render blockquotes. */
    char *got = render_one("intro\n> first quote line\n> second quote line\nback to prose");
    EXPECT_STR_EQ(got, "intro\n> first quote line\n> second quote line\nback to prose");
    free(got);
}

static void test_hard_break_blockquote_preserves_continuation_indent(void)
{
    /* A line after a `>` blockquote may have its own indent that we
     * should preserve — the `cur_line_is_block` hard-break path
     * doesn't normalize leading spaces because no marker matched. */
    char *got = render_one("> quote\n  continuation");
    EXPECT_STR_EQ(got, "> quote\n  continuation");
    free(got);
}

static void test_hard_break_table_preserves_continuation_indent(void)
{
    /* Same for tables — a line after a `|` row preserves its
     * indent rather than being flattened. */
    char *got = render_one("| cell |\n  continuation");
    EXPECT_STR_EQ(got, "| cell |\n  continuation");
    free(got);
}

static void test_hard_break_thematic_dashes(void)
{
    /* `---` / `***` / `___` is a thematic break (or setext underline)
     * — must stay on its own line. */
    char *got = render_one("section\n---\nnext bit");
    EXPECT_STR_EQ(got, "section\n---\nnext bit");
    free(got);
}

static void test_hard_break_thematic_stars(void)
{
    char *got = render_one("section\n***\nnext bit");
    EXPECT_STR_EQ(got, "section\n***\nnext bit");
    free(got);
}

static void test_hard_break_setext_h1_underline(void)
{
    /* `=====` setext h1 underline — we don't render setext headings
     * specially, but the underline must stay on its own line. */
    char *got = render_one("Heading\n=====\nbody");
    EXPECT_STR_EQ(got, "Heading\n=====\nbody");
    free(got);
}

static void test_hard_break_spaced_thematic_underscores(void)
{
    /* CommonMark allows whitespace between thematic markers:
     * `_ _ _` is a thematic break. Soft-join must not collapse it. */
    char *got = render_one("section\n_ _ _\nnext");
    EXPECT_STR_EQ(got, "section\n_ _ _\nnext");
    free(got);
}

static void test_hard_break_spaced_thematic_stars(void)
{
    char *got = render_one("section\n* * *\nnext");
    /* Bullet check fires first on `* * *` since data[scan+1]=' '
     * matches the `* ` bullet pattern. The hard break still
     * happens, which is what matters; step_line_start's thematic
     * consumer then captures the full marker line. */
    EXPECT_STR_EQ(got, "section\n* * *\nnext");
    free(got);
}

static void test_hard_break_thematic_underscores(void)
{
    char *got = render_one("section\n___\nnext bit");
    EXPECT_STR_EQ(got, "section\n___\nnext bit");
    free(got);
}

static void test_soft_join_double_dash_is_not_thematic(void)
{
    /* `--flag` (only two dashes) is prose — should soft-join. The
     * thematic check requires three of the same char. */
    char *got = render_one("use\n--verbose for more");
    EXPECT_STR_EQ(got, "use --verbose for more");
    free(got);
}

static void test_hard_break_plus_bullet(void)
{
    char *got = render_one("intro\n+ alpha\n+ beta");
    EXPECT_STR_EQ(got, "intro\n+ alpha\n+ beta");
    free(got);
}

static void test_hard_break_numbered_paren(void)
{
    /* `N)` numbered list marker, alongside `N.`. */
    char *got = render_one("intro\n1) alpha\n2) beta");
    EXPECT_STR_EQ(got, "intro\n1) alpha\n2) beta");
    free(got);
}

static void test_hard_break_table_row(void)
{
    /* GFM table — each row starts with `|`. We don't pretty-print
     * the table, but the soft-join must NOT collapse rows into one
     * line, otherwise the cell delimiters end up jumbled together. */
    const char *in = "| Tool | Does |\n|---|---|\n| read | Read files |\n| bash | Run shell |";
    char *got = render_one(in);
    EXPECT_STR_EQ(got, in);
    free(got);
}

static void test_code_fence_no_soft_join(void)
{
    /* Inside a code fence, \n stays \n — fences are verbatim. */
    char *got = render_one("```\nline1\nline2\n```\n");
    EXPECT_STR_EQ(got, "\x1b[2mline1\nline2\n\x1b[22m");
    free(got);
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

/* ---------- wrap mode ---------- */

/* Render with wrap enabled at the given cell budget. */
static char *render_wrap(const char *input, int wrap_width)
{
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, wrap_width);
    md_feed(m, input, strlen(input));
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    return s ? s : xstrdup("");
}

static void test_wrap_disabled_byte_exact(void)
{
    /* wrap_width=0 must be byte-identical to the legacy renderer
     * (the rest of the suite already verifies this — this test just
     * documents the contract: 0 disables, anything else wraps). */
    char *got = render_wrap("foo bar baz qux quux", 0);
    EXPECT_STR_EQ(got, "foo bar baz qux quux");
    free(got);
}

static void test_wrap_long_paragraph(void)
{
    /* 30-cell budget; full input is 39 cells. Break lands at the
     * last space at or before col 30 (the space after "amet,"). */
    char *got = render_wrap("Lorem ipsum dolor sit amet, consectetur", 30);
    EXPECT_STR_EQ(got, "Lorem ipsum dolor sit amet,\nconsectetur");
    free(got);
}

static void test_wrap_multiple_breaks(void)
{
    /* Three breaks at a 12-cell budget. */
    char *got = render_wrap("one two three four five six seven", 12);
    EXPECT_STR_EQ(got, "one two\nthree four\nfive six\nseven");
    free(got);
}

static void test_wrap_under_budget_unchanged(void)
{
    char *got = render_wrap("short text", 80);
    EXPECT_STR_EQ(got, "short text");
    free(got);
}

static void test_wrap_tab_collapsed_to_single_space(void)
{
    /* Wrap branch substitutes \t with a single space (not 4 — see
     * emit_text comment: 4 spaces would let stream-commits push the
     * row past wrap_width). Mid-prose tabs are vanishingly rare from
     * models; collapsing to one space is semantically equivalent for
     * prose. */
    char *got = render_wrap("a\tb", 80);
    EXPECT_STR_EQ(got, "a b");
    free(got);
}

static void test_wrap_tab_near_right_edge_no_overflow(void)
{
    /* Regression for the stream-commit bug: with the 4-space tab
     * substitution, "ab\tcd" at a 4-cell budget would emit "ab   \ncd"
     * — first row 5 cells, over budget by 1, because three of the
     * four expanded spaces got stream-committed before any non-space
     * could trigger wrap_break. Single-space substitution lets the
     * break fire cleanly at the tab. */
    char *got = render_wrap("ab\tcd", 4);
    EXPECT_STR_EQ(got, "ab\ncd");
    free(got);
}

static void test_wrap_long_word_overflow(void)
{
    /* A single word longer than the budget — long-word fallback emits
     * the row as-is (over budget) and continues on the next row. */
    char *got = render_wrap("supercalifragilistic foo", 10);
    /* "supercalifragilistic" is 20 cells, no break possible. After
     * emitting, " foo" follows in the next row. */
    EXPECT_STR_EQ(got, "supercalifragilistic\nfoo");
    free(got);
}

static void test_wrap_list_indent(void)
{
    /* Bullet list: continuation row indents under the marker. The
     * marker itself ("* ") is 2 cells; budget 30 leaves 28 for first
     * row, 28 (plus 2 indent = 30) for continuation. */
    char *got = render_wrap("* alpha beta gamma delta epsilon zeta", 20);
    EXPECT_STR_EQ(got, "* alpha beta gamma\n  delta epsilon zeta");
    free(got);
}

static void test_wrap_numbered_list_indent(void)
{
    /* "1. " marker is 3 cells, so continuation indent is 3. */
    char *got = render_wrap("1. alpha beta gamma delta epsilon", 20);
    EXPECT_STR_EQ(got, "1. alpha beta gamma\n   delta epsilon");
    free(got);
}

static void test_wrap_nested_bullet_hanging_indent(void)
{
    /* Nested list: parent at col 0, child indented 2. The child's
     * wrap continuation must indent under its own content column
     * (col 4 = 2 leading spaces + "- ") rather than col 0. The
     * continuation budget can overshoot wrap_width by up to 20-cell
     * floor (current_row_budget) so deeply-indented rows still get
     * a viable amount of content per row. */
    const char *in = "* parent\n  - child alpha beta gamma delta epsilon zeta";
    char *got = render_wrap(in, 22);
    EXPECT_STR_EQ(got, "* parent\n"
                       "  - child alpha beta\n"
                       "    gamma delta epsilon\n"
                       "    zeta");
    free(got);
}

static void test_wrap_long_numbered_marker_indent(void)
{
    /* 4-digit marker "1000. " is 6 cells. Continuation rows must
     * indent under the content column, not collapse to 0. */
    char *got = render_wrap("1000. alpha beta gamma delta epsilon zeta", 24);
    EXPECT_STR_EQ(got, "1000. alpha beta gamma\n      delta epsilon zeta");
    free(got);
}

static void test_wrap_dash_list_indent(void)
{
    char *got = render_wrap("- alpha beta gamma delta epsilon", 20);
    EXPECT_STR_EQ(got, "- alpha beta gamma\n  delta epsilon");
    free(got);
}

static void test_wrap_soft_join_then_wrap(void)
{
    /* Model emitted narrow lines; the soft-join collapses them to a
     * paragraph and the wrap layer re-flows at our width. */
    char *got = render_wrap("alpha beta\ngamma delta\nepsilon zeta", 20);
    EXPECT_STR_EQ(got, "alpha beta gamma\ndelta epsilon zeta");
    free(got);
}

static void test_wrap_preserves_hard_break(void)
{
    /* A blank line between paragraphs is hard — no merging. */
    char *got = render_wrap("foo bar baz\n\nqux quux", 30);
    EXPECT_STR_EQ(got, "foo bar baz\n\nqux quux");
    free(got);
}

static void test_wrap_skips_code_fence(void)
{
    /* Code fences pass content through verbatim — no wrap, no
     * column tracking. The dim escape and \n are produced as today. */
    const char *in = "```\nthis is a deliberately long code line that exceeds the budget\n```\n";
    char *got = render_wrap(in, 20);
    const char *want =
        "\x1b[2mthis is a deliberately long code line that exceeds the budget\n\x1b[22m";
    EXPECT_STR_EQ(got, want);
    free(got);
}

static void test_wrap_carries_bold_across_break(void)
{
    /* Bold escapes are buffered with content so the row-break emits
     * them in the right order. The ANSI bold-on goes out before the
     * first row, bold-off after the second — terminal preserves SGR
     * across the inserted \n. */
    char *got = render_wrap("**alpha beta gamma delta**", 15);
    /* Break lands after "alpha beta" (10 cells in bold). Continuation
     * has "gamma delta" with bold still on; closer follows. */
    EXPECT_STR_EQ(got, "\x1b[1malpha beta\ngamma delta\x1b[22m");
    free(got);
}

static void test_wrap_inline_code_breaks_at_space(void)
{
    /* Inline code spans aren't atomic — a soft break inside the span
     * is preferred over an overshooting row, since SGR persists
     * across \n so the visual cyan run stays continuous. The cyan
     * opens on row 1, the closer (FG_DEFAULT) lands on row 2 after
     * the carried-over content. */
    char *got = render_wrap("foo `code with spaces` bar baz", 15);
    EXPECT_STR_EQ(got, "foo \x1b[36mcode with\nspaces\x1b[39m bar baz");
    free(got);
}

static void test_wrap_multibyte_codepoints(void)
{
    /* "héllo" is 5 cells (é is 1 cell, 2 bytes). At budget 6 we fit;
     * "héllo wörld" is 11 cells — wraps after "héllo". */
    char *got = render_wrap("h\xC3\xA9llo w\xC3\xB6rld", 8);
    EXPECT_STR_EQ(got, "h\xC3\xA9llo\nw\xC3\xB6rld");
    free(got);
}

static void test_wrap_partial_utf8_drains_before_hard_newline(void)
{
    /* If a malformed UTF-8 lead byte is pending when a hard \n
     * arrives, it must be flushed BEFORE the \n in output — not
     * carried forward to combine with the next line's first byte. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 80);
    md_feed(m, "A\xC3", 2); /* lead byte with no continuation */
    md_feed(m, "\n\nX", 3); /* hard breaks should not reorder past the \xC3 */
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    /* The orphan \xC3 stays adjacent to the 'A' on the first line. */
    EXPECT_STR_EQ(s, "A\xC3\n\nX");
    free(s);
}

static void test_wrap_partial_utf8_drains_before_raw(void)
{
    /* Same ordering invariant when the next emit is an ANSI raw
     * escape from a marker transition. Pending malformed bytes
     * must precede the escape so the output stays in source order. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 80);
    md_feed(m, "A\xC3", 2);
    md_feed(m, "**B**", 5);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "A\xC3\x1b[1mB\x1b[22m");
    free(s);
}

static void test_wrap_partial_utf8_across_feeds(void)
{
    /* Multi-byte sequence split across feeds: the wrap layer's
     * cp_stream must hold partial bytes until the codepoint is
     * complete, so col counting stays correct. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 12);
    md_feed(m, "h\xC3", 2);
    md_feed(m, "\xA9llo wonderful world", 21);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "h\xC3\xA9llo\nwonderful\nworld");
    free(s);
}

static void test_wrap_soft_wrapped_list_item(void)
{
    /* Model emitted a list item with an internal soft break (no
     * marker on the continuation line). The renderer must fold the
     * \n into a space and re-flow the joined item at our width with
     * the correct hanging indent under the bullet. */
    char *got = render_wrap("* alpha beta gamma delta\nepsilon zeta eta theta iota", 20);
    EXPECT_STR_EQ(got, "* alpha beta gamma\n  delta epsilon zeta\n  eta theta iota");
    free(got);
}

static void test_soft_join_indented_continuation(void)
{
    /* CommonMark "lazy continuation" idiom: an item's body wrapped by
     * the model with the continuation line indented under the
     * marker's content column. The leading whitespace on line 2 must
     * be absorbed and the join must produce exactly one space — not
     * two (one from the \n, one from the leading indent). Same for
     * the second item. Wrap is disabled here so we verify the join
     * shape without the wrap layer's re-flow obscuring things. */
    const char *in = "* list item\n  indented continuation\n* another item\n  another indent";
    char *got = render_one(in);
    EXPECT_STR_EQ(got, "* list item indented continuation\n* another item another indent");
    free(got);
}

static void test_wrap_indented_continuation_reflows(void)
{
    /* Same input, wrap enabled at 22 cells: each item joins into a
     * single logical line, then re-flows with hanging indent 2. */
    const char *in = "* list item\n  indented continuation\n* another item\n  another indent";
    char *got = render_wrap(in, 22);
    EXPECT_STR_EQ(got, "* list item indented\n"
                       "  continuation\n"
                       "* another item another\n"
                       "  indent");
    free(got);
}

/* Capture every emit_cb call as a separate (kind, bytes) record so
 * tests can assert that content streams progressively instead of
 * landing in one final flush. The byte stream is identical to what
 * `capture` would produce — this just preserves call boundaries. */
struct stream_capture {
    char buf[1024];
    size_t len;
    int call_count;
    /* Length of buf after each call, for reconstructing what the
     * terminal saw at each emit boundary. */
    size_t lens[32];
};

static void stream_capture_cb(const char *bytes, size_t n, int is_raw, void *user)
{
    (void)is_raw;
    struct stream_capture *c = user;
    if (c->len + n < sizeof(c->buf)) {
        memcpy(c->buf + c->len, bytes, n);
        c->len += n;
    }
    if (c->call_count < (int)(sizeof(c->lens) / sizeof(c->lens[0])))
        c->lens[c->call_count] = c->len;
    c->call_count++;
}

static void test_wrap_streams_completed_words(void)
{
    /* With wrap enabled, content should reach emit_cb as words arrive
     * (committed through the previous break) — not all at once when
     * the row wraps or the stream flushes. After feeding "alpha "
     * "beta " "gamma" with budget 100 (no wrap), the renderer should
     * have committed at least "alpha" before md_flush runs. */
    struct stream_capture c = {0};
    struct md_renderer *m = md_new(stream_capture_cb, &c, 100);
    md_feed(m, "alpha ", 6);
    md_feed(m, "beta ", 5);
    /* At this point the second space has arrived — "alpha " should
     * already be committed even though md_flush hasn't run. */
    EXPECT(c.len >= 6);
    EXPECT(memcmp(c.buf, "alpha ", 6) == 0);
    md_feed(m, "gamma", 5);
    md_flush(m);
    md_free(m);
    EXPECT(c.len == 16);
    EXPECT(memcmp(c.buf, "alpha beta gamma", 16) == 0);
}

static void test_wrap_heading_not_wrapped(void)
{
    /* Headings are single-line by policy — long heading content
     * passes through verbatim even when wrap is active. */
    char *got = render_wrap("## A very long heading that exceeds the budget\n", 20);
    EXPECT_STR_EQ(got, "\x1b[1mA very long heading that exceeds the budget\x1b[22m\n");
    free(got);
}

int main(void)
{
    /* Wrap mode measures cells via utf8_codepoint_cells which needs
     * a UTF-8 LC_CTYPE for multibyte decoding. The non-wrap tests
     * (wrap_width=0) don't strictly need it but initializing here is
     * cheap and harmless. */
    locale_init_utf8();

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
    test_code_fence_tab_expanded_to_four_spaces();
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

    test_soft_join_user_review_pattern();
    test_eof_thematic_no_trailing_newline();
    test_eof_setext_h1_no_trailing_newline();
    test_eof_setext_h2_no_trailing_newline();
    test_eof_thematic_spaced_no_trailing_newline();
    test_crlf_thematic_break();
    test_eof_soft_join_digits();
    test_eof_soft_join_dash_alone();
    test_eof_soft_join_hash_alone();
    test_eof_hard_break_blank();
    test_soft_break_simple_join();
    test_soft_break_skip_leading_whitespace();
    test_soft_break_no_double_space();
    test_hard_break_blank_line();
    test_hard_break_blank_line_4_spaces();
    test_hard_break_blank_line_with_tab();
    test_soft_join_hash_no_space();
    test_line_start_hash_no_space();
    test_hard_break_list_marker();
    test_hard_break_dash_list();
    test_hard_break_numbered_list();
    test_hard_break_heading();
    test_hard_break_fence();
    test_soft_break_carries_bold();
    test_soft_break_across_feeds();
    test_soft_break_paragraph_break_across_feeds();
    test_code_fence_no_soft_join();
    test_hard_break_table_row();
    test_hard_break_indented_bullet_preserves_indent();
    test_hard_break_indented_bullet_no_parent();
    test_hard_break_indented_heading();
    test_hard_break_indented_fence();
    test_soft_join_4_leading_spaces_not_marker();
    test_line_start_indented_heading();
    test_line_start_indented_fence();
    test_line_start_indented_thematic();
    test_line_start_indented_after_blank();
    test_line_start_indented_bullet_preserves();
    test_hard_break_blockquote();
    test_hard_break_blockquote_preserves_continuation_indent();
    test_hard_break_table_preserves_continuation_indent();
    test_hard_break_thematic_dashes();
    test_hard_break_thematic_stars();
    test_hard_break_thematic_underscores();
    test_hard_break_setext_h1_underline();
    test_hard_break_spaced_thematic_underscores();
    test_hard_break_spaced_thematic_stars();
    test_soft_join_double_dash_is_not_thematic();
    test_hard_break_plus_bullet();
    test_hard_break_numbered_paren();

    test_real_world_paragraph();

    test_wrap_disabled_byte_exact();
    test_wrap_long_paragraph();
    test_wrap_multiple_breaks();
    test_wrap_under_budget_unchanged();
    test_wrap_tab_collapsed_to_single_space();
    test_wrap_tab_near_right_edge_no_overflow();
    test_wrap_long_word_overflow();
    test_wrap_list_indent();
    test_wrap_numbered_list_indent();
    test_wrap_long_numbered_marker_indent();
    test_wrap_nested_bullet_hanging_indent();
    test_wrap_dash_list_indent();
    test_wrap_soft_join_then_wrap();
    test_wrap_preserves_hard_break();
    test_wrap_skips_code_fence();
    test_wrap_carries_bold_across_break();
    test_wrap_inline_code_breaks_at_space();
    test_wrap_multibyte_codepoints();
    test_wrap_partial_utf8_drains_before_hard_newline();
    test_wrap_partial_utf8_drains_before_raw();
    test_wrap_partial_utf8_across_feeds();
    test_wrap_soft_wrapped_list_item();
    test_soft_join_indented_continuation();
    test_wrap_indented_continuation_reflows();
    test_wrap_streams_completed_words();
    test_wrap_heading_not_wrapped();

    T_REPORT();
}
