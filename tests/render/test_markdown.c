/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "util.h"
#include "render/markdown.h"
#include "terminal/ansi.h"

/* Building blocks for expected-output strings. The SGR macros alias the
 * canonical ANSI_* sequences (single source of truth in terminal/ansi.h);
 * the rest are content glyphs the renderer emits, which aren't ANSI
 * escapes so they have no ANSI_* equivalent. */
#define DIM      ANSI_DIM                        /* \x1b[2m */
#define OFF      ANSI_BOLD_OFF                   /* \x1b[22m — SGR 22 closes bold AND dim */
#define BLD      ANSI_BOLD                       /* \x1b[1m */
#define ITAL     ANSI_ITALIC                     /* \x1b[3m */
#define ITAL_OFF ANSI_ITALIC_OFF                 /* \x1b[23m */
#define CODE     ANSI_CYAN                       /* \x1b[36m — inline code span */
#define CODE_OFF ANSI_FG_DEFAULT                 /* \x1b[39m */
#define ERASE    ANSI_ERASE_LINE                 /* \x1b[K */
#define CUB(n)   "\x1b[" #n "D"                  /* cursor back n columns (retro-wrap) */
#define BUL      DIM "\xe2\x80\xa2 " OFF         /* dim "• " bullet */
#define DOT      "\xc2\xb7"                      /* · middle dot (divider) */
#define DINKUS   DIM DOT "   " DOT "   " DOT OFF /* 3-dot divider (wide/unlimited width) */
#define HL       "\xe2\x94\x80"                  /* ─ table rule */
#define VB       "\xe2\x94\x82"                  /* │ table column separator */
#define CR       "\xe2\x94\xbc"                  /* ┼ table underline crossing */
#define TSEP     " " DIM VB OFF " "              /* dim " │ " column separator */

/* Capture md_renderer output into a buf so tests can compare bytes. The
 * is_raw flag distinguishes ANSI escapes from content for downstream
 * routing in production; tests don't care, so we collapse both into one
 * stream of bytes and verify the literal byte sequence. */
static void capture(const char *bytes, size_t n, int is_raw, void *user)
{
    (void)is_raw;
    buf_append((struct buf *)user, bytes, n);
}

/* `text` / `raw` support direct assertions; `wire` preserves callback order
 * and `kinds` tags each corresponding byte (0 = content, 1 = raw). Keeping
 * both views lets validation ignore callback boundaries, which may split CSI. */
struct kind_capture {
    struct buf text;
    struct buf raw;
    struct buf wire;
    struct buf kinds;
};

/* Validate the reassembled stream: complete CSI sequences must be entirely
 * raw, while every byte outside them must be content. */
static int capture_kinds_valid(const struct kind_capture *c)
{
    if (c->wire.len != c->kinds.len)
        return 0;
    size_t i = 0;
    while (i < c->wire.len) {
        if (c->wire.data[i] != '\x1b') {
            if (c->kinds.data[i] != 0)
                return 0;
            i++;
            continue;
        }
        size_t start = i;
        if (i + 2 > c->wire.len || c->wire.data[i + 1] != '[')
            return 0;
        i += 2;
        /* CSI parameter/intermediate bytes precede one final byte. */
        while (i < c->wire.len && (unsigned char)c->wire.data[i] >= 0x20 &&
               (unsigned char)c->wire.data[i] <= 0x3f)
            i++;
        if (i >= c->wire.len || (unsigned char)c->wire.data[i] < 0x40 ||
            (unsigned char)c->wire.data[i] > 0x7e)
            return 0;
        i++;
        for (size_t j = start; j < i; j++)
            if (c->kinds.data[j] != 1)
                return 0;
    }
    return 1;
}

static void capture_kind(const char *bytes, size_t n, int is_raw, void *user)
{
    struct kind_capture *c = user;
    buf_append(is_raw ? &c->raw : &c->text, bytes, n);
    buf_append(&c->wire, bytes, n);
    char kind = is_raw ? 1 : 0;
    for (size_t i = 0; i < n; i++)
        buf_append(&c->kinds, &kind, 1);
}

/* Common helper: feed the whole input as one block, flush, return string.
 * buf_steal returns NULL if nothing was appended; coerce to "" so callers
 * can EXPECT_STR_EQ unconditionally. */
static char *render_width(const char *input, int wrap_width)
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

static char *render_one(const char *input)
{
    return render_width(input, 0);
}

static char *render_split(const char *input, int wrap_width, size_t split)
{
    struct buf out;
    buf_init(&out);
    size_t len = strlen(input);
    struct md_renderer *m = md_new(capture, &out, wrap_width);
    md_feed(m, input, split);
    md_feed(m, input + split, len - split);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    return s ? s : xstrdup("");
}

static char *render_bytewise(const char *input, int wrap_width)
{
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, wrap_width);
    for (size_t i = 0; input[i]; i++)
        md_feed(m, input + i, 1);
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

/* ---------- blank-line collapsing ---------- */

static void test_single_blank_line_preserved(void)
{
    /* One blank line between paragraphs is the canonical gap — kept. */
    char *got = render_one("a\n\nb");
    EXPECT_STR_EQ(got, "a\n\nb");
    free(got);
}

static void test_multiple_blank_lines_collapse(void)
{
    /* Several blank lines collapse to a single-line gap. */
    char *got = render_one("a\n\n\n\nb");
    EXPECT_STR_EQ(got, "a\n\nb");
    free(got);
}

static void test_leading_blank_lines_stripped(void)
{
    /* Blank lines before the first content have no gap to open. */
    char *got = render_one("\n\n\ntext");
    EXPECT_STR_EQ(got, "text");
    free(got);
}

static void test_blank_lines_preserved_in_code_fence(void)
{
    /* Fenced code is verbatim — its interior blank lines survive. */
    char *got = render_one("```\nfoo\n\n\nbar\n```\n");
    EXPECT_STR_EQ(got, DIM "foo\n\n\nbar\n" OFF);
    free(got);
}

static void test_collapse_blank_lines_split_across_feeds(void)
{
    /* The blank run straddles a feed boundary — the boundary flag carries
     * across feeds so the gap still collapses to one line. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 0);
    md_feed(m, "a\n\n", 3);
    md_feed(m, "\n\nb", 3);
    md_flush(m);
    md_free(m);
    char *got = buf_steal(&out);
    EXPECT_STR_EQ(got, "a\n\nb");
    free(got);
}

/* ---------- bold ---------- */

static void test_bold_simple(void)
{
    char *got = render_one("**hi**");
    EXPECT_STR_EQ(got, BLD "hi" OFF);
    free(got);
}

static void test_bold_in_sentence(void)
{
    char *got = render_one("a **b** c");
    EXPECT_STR_EQ(got, "a " BLD "b" OFF " c");
    free(got);
}

/* ---------- italic ---------- */

static void test_italic_star(void)
{
    char *got = render_one("a *b* c");
    EXPECT_STR_EQ(got, "a " ITAL "b" ITAL_OFF " c");
    free(got);
}

static void test_italic_underscore(void)
{
    char *got = render_one("_b_");
    EXPECT_STR_EQ(got, ITAL "b" ITAL_OFF);
    free(got);
}

/* ---------- inline code ---------- */

static void test_inline_code(void)
{
    char *got = render_one("a `read` b");
    EXPECT_STR_EQ(got, "a " CODE "read" CODE_OFF " b");
    free(got);
}

/* ---------- the weird case: bold-around-code ---------- */

static void test_bold_around_code(void)
{
    /* **`read`** must produce bold-on, cyan-on, "read", cyan-off, bold-off.
     * The verbatim-cyan "read" comes through because inline-code spans
     * don't process inner markers. */
    char *got = render_one("**`read`**");
    EXPECT_STR_EQ(got, BLD CODE "read" CODE_OFF OFF);
    free(got);
}

/* ---------- headings ---------- */

static void test_heading_h3(void)
{
    char *got = render_one("### What it does\n");
    EXPECT_STR_EQ(got, BLD "What it does" OFF "\n");
    free(got);
}

static void test_heading_then_paragraph(void)
{
    char *got = render_one("## Tech\nDetails");
    EXPECT_STR_EQ(got, BLD "Tech" OFF "\nDetails");
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
    EXPECT_STR_EQ(got, DIM "foo\nbar\n" OFF);
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
    /* Code fence with a language identifier — only the fenced
     * content is rendered. */
    char *got = render_one("```c\nint x;\n```\n");
    EXPECT_STR_EQ(got, DIM "int x;\n" OFF);
    free(got);
}

static void test_code_fence_lang_with_attrs(void)
{
    /* Fence opener with extra info-string tokens — the entire opener
     * line is consumed and only fenced content is rendered. */
    char *got = render_one("```python title=\"x.py\"\nprint(1)\n```\n");
    EXPECT_STR_EQ(got, DIM "print(1)\n" OFF);
    free(got);
}

static void test_code_fence_lang_split_across_feeds(void)
{
    /* Fence opener spans feeds — the opener defers until we have \n,
     * so the fence opens correctly on the second feed. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 0);
    md_feed(m, "```py", 5);
    md_feed(m, "thon\nint x;\n```\n", 16);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, DIM "int x;\n" OFF);
    free(s);
}

static void test_code_fence_inner_markers_verbatim(void)
{
    /* `*` inside a fence must NOT toggle italic; the fence is verbatim. */
    char *got = render_one("```\n*not italic*\n```\n");
    EXPECT_STR_EQ(got, DIM "*not italic*\n" OFF);
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
    const char *want = DIM "# Title\n```python\ndef f(): pass\n" OFF;
    EXPECT_STR_EQ(got, want);
    free(got);
}

static void test_code_fence_four_backticks_wraps_three(void)
{
    /* A 4-backtick fence stays open across inner ``` lines — common
     * pattern for documenting code that itself contains backticks. */
    const char *in = "````\nthis has ```inner``` text\n````\n";
    char *got = render_one(in);
    const char *want = DIM "this has ```inner``` text\n" OFF;
    EXPECT_STR_EQ(got, want);
    free(got);
}

static void test_code_fence_three_backticks_dont_close_four(void)
{
    /* A 3-backtick line inside a 4-backtick fence stays as content. */
    const char *in = "````\nbody\n```\nstill body\n````\n";
    char *got = render_one(in);
    const char *want = DIM "body\n```\nstill body\n" OFF;
    EXPECT_STR_EQ(got, want);
    free(got);
}

static void test_code_fence_closer_with_trailing_spaces(void)
{
    /* CommonMark allows trailing whitespace on the closer line. */
    char *got = render_one("```\nfoo\n```   \n");
    EXPECT_STR_EQ(got, DIM "foo\n" OFF);
    free(got);
}

static void test_code_fence_crlf_closer(void)
{
    /* CRLF line endings — the trailing \r before \n must be treated as
     * whitespace, otherwise the closer is misread as content. */
    char *got = render_one("```\r\nfoo\r\n```\r\n");
    EXPECT_STR_EQ(got, DIM "foo\r\n" OFF);
    free(got);
}

static void test_code_fence_crlf_lang(void)
{
    /* CRLF on the opener line — \r before \n is treated as whitespace
     * and does not leak into rendered output. */
    char *got = render_one("```python\r\nx = 1\r\n```\r\n");
    EXPECT_STR_EQ(got, DIM "x = 1\r\n" OFF);
    free(got);
}

/* ---------- list markers ---------- */

static void test_star_space_is_list_marker(void)
{
    /* `* foo` at line start renders as a dim bullet — must not enter italic. */
    char *got = render_one("* foo");
    EXPECT_STR_EQ(got, BUL "foo");
    free(got);
}

static void test_dash_list_passthrough(void)
{
    /* `- foo` renders as a dim bullet. */
    char *got = render_one("- foo");
    EXPECT_STR_EQ(got, BUL "foo");
    free(got);
}

static void test_numbered_list_passthrough(void)
{
    char *got = render_one("1. foo\n2. bar");
    EXPECT_STR_EQ(got, DIM "1. " OFF "foo\n" DIM "2. " OFF "bar");
    free(got);
}

static void test_list_with_bold(void)
{
    /* Snippet pattern: `- **bold**: rest` */
    char *got = render_one("- **bold**: rest");
    EXPECT_STR_EQ(got, BUL BLD "bold" OFF ": rest");
    free(got);
}

static void test_bullet_collapses_padded_marker(void)
{
    /* Some models pad the marker for alignment (`-   foo`); the extra
     * spaces collapse to the bullet's own single space. */
    char *got = render_one("-   foo");
    EXPECT_STR_EQ(got, BUL "foo");
    free(got);
}

static void test_numbered_collapses_padded_marker(void)
{
    /* Ordered marker padded for alignment (`1.  foo`) collapses to a
     * single separating space, so the first line aligns with the
     * hanging indent used for wrapped continuation rows. */
    char *got = render_one("1.  foo\n10. bar");
    EXPECT_STR_EQ(got, DIM "1. " OFF "foo\n" DIM "10. " OFF "bar");
    free(got);
}

static void test_padded_marker_indent_preserved(void)
{
    /* Leading indent survives the collapse of the trailing padding. */
    char *got = render_one("  -   foo");
    EXPECT_STR_EQ(got, "  " BUL "foo");
    free(got);
}

static void test_padded_marker_split_across_feeds(void)
{
    /* The padding run straddles a feed boundary: the parser must defer
     * until it sees the run end, then collapse it — not treat the
     * second feed's leading spaces as content. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 0);
    md_feed(m, "1.  ", 4); /* delimiter + spaces, run reaches buffer end */
    md_feed(m, "  foo", 5);
    md_flush(m);
    md_free(m);
    char *got = buf_steal(&out);
    EXPECT_STR_EQ(got, DIM "1. " OFF "foo");
    free(got);
}

/* An ordered marker with no item text renders eagerly at EOF: there's no
 * thematic-break ambiguity for digits, so the marker isn't deferred and the
 * padding (if any) still collapses to one space. */
static void test_numbered_marker_only_at_eof(void)
{
    char *got = render_one("1. ");
    EXPECT_STR_EQ(got, DIM "1. " OFF);
    free(got);
    got = render_one("1.   ");
    EXPECT_STR_EQ(got, DIM "1. " OFF);
    free(got);
    got = render_one("2) ");
    EXPECT_STR_EQ(got, DIM "2) " OFF);
    free(got);
}

static void test_bullet_marker_only_at_eof_stays_literal(void)
{
    /* A lone `-`/`*`/`_` + space at EOF can't be confirmed as a bullet: it
     * could still open a `---`/`***`/`___` thematic break once a newline
     * arrives, so the streaming parser defers it and, with no more input,
     * flush emits it literally. (Pre-existing behavior — the digit markers
     * above have no such ambiguity.) */
    char *got = render_one("- ");
    EXPECT_STR_EQ(got, "- ");
    free(got);
    got = render_one("*   ");
    EXPECT_STR_EQ(got, "*   ");
    free(got);
}

static void test_bare_marker_no_space_stays_literal(void)
{
    /* A marker char with no following space is not a list marker — it
     * stays literal at EOF, matching the streaming path. */
    char *got = render_one("-");
    EXPECT_STR_EQ(got, "-");
    free(got);
    got = render_one("1.");
    EXPECT_STR_EQ(got, "1.");
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
    EXPECT_STR_EQ(s, BLD "hi" OFF);
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
    EXPECT_STR_EQ(s, "x " CODE "y" CODE_OFF);
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
    EXPECT_STR_EQ(s, DIM "body\n" OFF);
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
    EXPECT_STR_EQ(s, BLD "title" OFF "\n");
    free(s);
}

/* ---------- feed partition invariance ---------- */

static void test_feed_partition_invariance(void)
{
    /* Provider deltas can split at any byte, so parsing and wrap wire output
     * must not depend on where feed boundaries happen to land. */
    static const struct {
        const char *input;
        int wrap_width;
    } cases[] = {
        {"plain **bold** and _italic_ with `code`", 0},
        {"first line\nsecond line\n\n  - **item**\n\n## Heading\n", 0},
        {"````markdown\n```c\nx = 1;\n```\n````\n", 0},
        {"| Name | Note |\n|---|---|\n| one | **two** |\n", 40},
        {"- alpha **bravo charlie** delta echo foxtrot", 16},
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        size_t len = strlen(cases[c].input);
        char *whole = render_width(cases[c].input, cases[c].wrap_width);
        for (size_t split = 0; split <= len; split++) {
            char *partitioned = render_split(cases[c].input, cases[c].wrap_width, split);
            EXPECT_STR_EQ(partitioned, whole);
            free(partitioned);
        }
        char *bytewise = render_bytewise(cases[c].input, cases[c].wrap_width);
        EXPECT_STR_EQ(bytewise, whole);
        free(bytewise);
        free(whole);
    }
}

static void test_feed_emits_prose_eagerly(void)
{
    /* Public feeds must emit prose without waiting for another delta or flush. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 100);

    md_feed(m, "alpha ", 6);
    EXPECT_MEM_EQ(out.data, out.len, "alpha ", 6);
    md_feed(m, "beta", 4);
    EXPECT_MEM_EQ(out.data, out.len, "alpha beta", 10);
    md_flush(m);
    EXPECT_MEM_EQ(out.data, out.len, "alpha beta", 10);

    md_free(m);
    buf_free(&out);
}

/* ---------- flush behavior ---------- */

static void test_flush_emits_unterminated_marker(void)
{
    /* A `**bold` with no closing — flush should emit the bold-on but
     * close it cleanly so the terminal isn't left in a styled state. */
    char *got = render_one("**bold");
    EXPECT_STR_EQ(got, BLD "bold" OFF);
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
    EXPECT_STR_EQ(got, ITAL "hi" ITAL_OFF);
    free(got);
}

static void test_flush_closes_underscore_italic_at_eof(void)
{
    char *got = render_one("_hi_");
    EXPECT_STR_EQ(got, ITAL "hi" ITAL_OFF);
    free(got);
}

static void test_flush_closes_bold_at_eof(void)
{
    /* `**hi**` with no trailing byte after the second `**` — the closer
     * defers and md_flush must recognize the bold close. */
    char *got = render_one("**hi**");
    EXPECT_STR_EQ(got, BLD "hi" OFF);
    free(got);
}

static void test_flush_closes_fence_at_eof(void)
{
    /* Fenced block with no trailing newline after the closing ``` — the
     * backticks defer (couldn't see whether more would follow) and
     * md_flush must recognize them as a valid closer. */
    char *got = render_one("```\nfoo\n```");
    EXPECT_STR_EQ(got, DIM "foo\n" OFF);
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

/* ---------- emit callback contract ---------- */

static void test_emit_kind_separates_content_and_sgr(void)
{
    /* Downstream display bookkeeping counts visible bytes and held newlines;
     * labeling SGR as content would corrupt those counts. */
    struct kind_capture c = {0};
    buf_init(&c.text);
    buf_init(&c.raw);
    struct md_renderer *m = md_new(capture_kind, &c, 0);
    md_feed(m, "**b** `c`", 9);
    md_flush(m);
    md_free(m);
    EXPECT(capture_kinds_valid(&c));
    EXPECT_STR_EQ(c.text.data, "b c");
    EXPECT_STR_EQ(c.raw.data, BLD OFF CODE CODE_OFF);
    buf_free(&c.text);
    buf_free(&c.raw);
    buf_free(&c.wire);
    buf_free(&c.kinds);
}

static void test_emit_kind_marks_wrap_cursor_control_raw(void)
{
    /* Retro-wrap CSI moves are zero-width terminal control, not rendered text,
     * and must bypass the same downstream bookkeeping as SGR. */
    struct kind_capture c = {0};
    buf_init(&c.text);
    buf_init(&c.raw);
    struct md_renderer *m = md_new(capture_kind, &c, 10);
    md_feed(m, "abc def ghi", 11);
    md_flush(m);
    md_free(m);
    EXPECT(capture_kinds_valid(&c));
    EXPECT_STR_EQ(c.text.data, "abc def gh\nghi");
    EXPECT_STR_EQ(c.raw.data, CUB(3) ERASE);
    buf_free(&c.text);
    buf_free(&c.raw);
    buf_free(&c.wire);
    buf_free(&c.kinds);
}

static void test_emit_kind_covers_table_paths(void)
{
    /* Aligned tables emit SGR directly and replay styled-cell metadata;
     * narrow tables replay the same cells through the wrap layer. */
    static const struct {
        const char *input;
        int wrap_width;
    } cases[] = {
        {"| H | V |\n|---|---|\n| x | **b** |\n", 40},
        {"| Key | Value |\n|---|---|\n| x | **alpha beta** |\n", 10},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct kind_capture c = {0};
        buf_init(&c.text);
        buf_init(&c.raw);
        struct md_renderer *m = md_new(capture_kind, &c, cases[i].wrap_width);
        md_feed(m, cases[i].input, strlen(cases[i].input));
        md_flush(m);
        md_free(m);
        EXPECT(capture_kinds_valid(&c));
        EXPECT(c.text.len > 0);
        EXPECT(c.raw.len > 0);
        EXPECT(strstr(c.raw.data, BLD) != NULL);
        buf_free(&c.text);
        buf_free(&c.raw);
        buf_free(&c.wire);
        buf_free(&c.kinds);
    }
}

/* ---------- public modes and state ---------- */

static void test_styled_mode_transition_soft_resets(void)
{
    /* The caller emits its own newline at a style seam, so md must reset to
     * matching line-start and wrap state. The heading checks parser state;
     * the edge-space before the second seam leaves a pending wrap to clear. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 5);
    md_set_styled(m, 0);
    md_feed(m, "**quiet**", 9);
    md_set_styled(m, 1);
    buf_append(&out, "\n", 1); /* caller-owned seam */
    md_feed(m, "## H\n", 5);

    md_set_styled(m, 0);
    md_feed(m, "abcde ", 6);
    md_set_styled(m, 1);
    buf_append(&out, "\n", 1); /* caller-owned seam */
    md_feed(m, "x", 1);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "quiet\n" BLD "H" OFF "\nabcde\nx");
    free(s);
}

static void test_in_table_tracks_buffering(void)
{
    /* The caller shows a composing spinner while tables silently buffer, so
     * md_in_table must cover exactly the interval in which no rows are emitted. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 40);
    EXPECT(md_in_table(m) == 0);
    md_feed(m, "| A |\n|---|\n", 12);
    EXPECT(md_in_table(m) == 1);
    EXPECT(out.len == 0);
    md_feed(m, "| x |\n", 6);
    EXPECT(md_in_table(m) == 1);
    EXPECT(out.len == 0);
    md_feed(m, "after\n", 6);
    EXPECT(md_in_table(m) == 0);
    EXPECT(out.len > 0);
    md_flush(m);

    buf_reset(&out);
    md_reset(m, 40);
    md_feed(m, "| A |\n|---|\n", 12);
    EXPECT(md_in_table(m) == 1);
    md_flush(m);
    EXPECT(md_in_table(m) == 0);
    EXPECT(out.len > 0);
    md_free(m);
    buf_free(&out);
}

/* ---------- markers inside code spans/fences must stay verbatim ---------- */

static void test_inline_code_bold_marker_verbatim(void)
{
    /* `**foo**` — bold-marker bytes inside backticks must not toggle bold. */
    char *got = render_one("see `**foo**` here");
    EXPECT_STR_EQ(got, "see " CODE "**foo**" CODE_OFF " here");
    free(got);
}

static void test_inline_code_underscore_verbatim(void)
{
    /* The user's reported case wrapped in inline code. */
    char *got = render_one("`compile_commands.json`");
    EXPECT_STR_EQ(got, CODE "compile_commands.json" CODE_OFF);
    free(got);
}

static void test_code_fence_all_markers_verbatim(void)
{
    char *got = render_one("```\n**bold** and _italic_ and `code`\n```\n");
    EXPECT_STR_EQ(got, DIM "**bold** and _italic_ and `code`\n" OFF);
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
    EXPECT_STR_EQ(got, ITAL "word" ITAL_OFF);
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
    EXPECT_STR_EQ(s, "say " ITAL "yes" ITAL_OFF);
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
    /* Indented `  * item` — line-start path catches this (up to 3 leading
     * spaces are allowed before a list marker), and renders it as a dim
     * bullet with the indent preserved. */
    char *got = render_one("  * item");
    EXPECT_STR_EQ(got, "  " BUL "item");
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
    /* Fence opener split mid-info across feeds — the deferred opener
     * must be buffered intact so the fence opens cleanly once the \n
     * arrives, not flushed as plain text (which would let the closing
     * ``` start a new empty fence). */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 0);
    md_feed(m, "```python title=\"some/very/long/", 32);
    md_feed(m, "path/here.py\"\nx = 1\n```\n", 25);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, DIM "x = 1\n" OFF);
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
    EXPECT_STR_EQ(got, BUL BLD "P3 " CODE "src/path:line" CODE_OFF " - lorem ipsum." OFF
                               " The detailed explanation follows.");
    free(got);
}

static void test_eof_thematic_no_trailing_newline(void)
{
    /* Thematic break at end of stream without a trailing \n. The
     * streaming \n handler defers waiting for the line terminator, but at
     * md_flush the bytes-we-have ARE the whole line, so a 3+ marker run is
     * a valid thematic break and renders as the dim divider — same as when
     * it has a trailing newline (no EOF-only literal inconsistency). */
    char *got = render_one("section\n---");
    EXPECT_STR_EQ(got, "section\n" DINKUS "\n");
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
    /* `-----` is both a setext h2 underline and a thematic break; we don't
     * render setext, so it becomes the dim divider — matching the streaming
     * path for `Heading\n-----\n`. */
    char *got = render_one("Heading\n-----");
    EXPECT_STR_EQ(got, "Heading\n" DINKUS "\n");
    free(got);
}

static void test_eof_thematic_spaced_no_trailing_newline(void)
{
    char *got = render_one("section\n_ _ _");
    EXPECT_STR_EQ(got, "section\n" DINKUS "\n");
    free(got);
}

static void test_eof_bare_thematic_no_trailing_newline(void)
{
    /* Input that is *only* a thematic break (no preceding line) and ends
     * without a newline still renders as the divider, not literal dashes. */
    char *got = render_one("---");
    EXPECT_STR_EQ(got, DINKUS "\n");
    free(got);
}

static void test_crlf_thematic_break(void)
{
    /* CRLF line endings — \r before \n in the marker line must be
     * treated as whitespace by the soft-break thematic scan, so
     * the marker is recognized and rendered as a dinkus. */
    char *got = render_one("section\n---\r\nnext");
    EXPECT_STR_EQ(got, "section\n" DIM "\xc2\xb7   \xc2\xb7   \xc2\xb7" OFF "\nnext");
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

static void test_eof_hard_break_trailing_spaces_ambiguous_prefix(void)
{
    /* Mirror of the soft-join-at-EOF cases above, but with two trailing
     * spaces: the streaming \n handler defers on the ambiguous prefix
     * (`#` / `-` / `2024`), and at EOF the trailing pair resolves it as
     * a hard line break rather than a soft join. */
    char *got = render_one("foo  \n#");
    EXPECT_STR_EQ(got, "foo  \n#");
    free(got);
    got = render_one("foo  \n-");
    EXPECT_STR_EQ(got, "foo  \n-");
    free(got);
    got = render_one("foo  \n2024");
    EXPECT_STR_EQ(got, "foo  \n2024");
    free(got);
}

static void test_hard_break_crlf(void)
{
    /* CRLF line ending: the \r is part of the terminator, so it must
     * not reset the trailing-space run before the \n is classified —
     * "foo  \r\nbar" is a hard break, like its \n-only counterpart. */
    char *got = render_one("foo  \r\nbar");
    EXPECT_STR_EQ(got, "foo  \r\nbar");
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

static void test_hard_break_trailing_two_spaces(void)
{
    /* CommonMark hard line break: 2+ spaces before \n force a real
     * break instead of a soft join. The trailing spaces stay on the
     * wire but, sitting at end-of-line before the \n, are invisible —
     * the bug this guards against was a soft join leaving them inline
     * as "foo  bar" (a stray double space). */
    char *got = render_one("foo  \nbar");
    EXPECT_STR_EQ(got, "foo  \nbar");
    free(got);
}

static void test_hard_break_trailing_two_spaces_after_code_span(void)
{
    /* The exact shape models emit: an inline code span closed right at
     * a 2-space hard break, then an indented continuation line. The
     * span must break onto its own line, not soft-join with a double
     * space before the next word. */
    char *got = render_one("`x`  \n  bar");
    EXPECT_STR_EQ(got, CODE "x" CODE_OFF "  \n  bar");
    free(got);
}

static void test_hard_break_trailing_spaces_split_across_feeds(void)
{
    /* The trailing spaces and the \n arrive in separate feeds. The
     * hard-break count is renderer state (m->trailing_spaces), so it
     * still fires — a buffer-only scan would miss it. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 0);
    md_feed(m, "foo  ", 5); /* content + 2 trailing spaces */
    md_feed(m, "\nbar", 4); /* newline + next line, separate feed */
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "foo  \nbar");
    free(s);
}

static void test_soft_break_single_trailing_space(void)
{
    /* A single trailing space is below the hard-break threshold, so the
     * line still soft-joins — and to exactly one space, not two. */
    char *got = render_one("foo \nbar");
    EXPECT_STR_EQ(got, "foo bar");
    free(got);
}

static void test_hard_break_carries_emphasis(void)
{
    /* A hard line break is inline content, so open emphasis continues
     * across it (CommonMark) — bold must not close at the break, and
     * the trailing `**` is consumed as the closer rather than left
     * literal. */
    char *got = render_one("**foo  \nbar**");
    EXPECT_STR_EQ(got, BLD "foo  \nbar" OFF);
    free(got);
}

static void test_trailing_spaces_before_delimiter_soft_join(void)
{
    /* The two spaces sit before the closing `**`, not before the \n, so
     * the line ends with a delimiter — a soft join, not a hard break.
     * The consumed delimiter clears the trailing-space run. */
    char *got = render_one("**foo  **\nbar");
    EXPECT_STR_EQ(got, BLD "foo  " OFF " bar");
    free(got);
}

static void test_hard_break_before_block_closes_emphasis(void)
{
    /* When the next line opens a block (here a blank-line paragraph
     * break), the block path wins over the inline hard line break and
     * closes emphasis — identical to the same input without the
     * trailing spaces. The hard *line* break only carries emphasis
     * mid-paragraph. */
    char *got = render_one("**foo  \n\nbar**");
    EXPECT_STR_EQ(got, BLD "foo  " OFF "\n\nbar**");
    free(got);
}

static void test_hard_break_before_fence_closes_emphasis(void)
{
    /* Same rule with a fence opener as the next line: emphasis closes
     * before the fence rather than being carried into the code block. */
    char *got = render_one("**foo  \n```\ncode\n```");
    EXPECT_STR_EQ(got, BLD "foo  " OFF "\n" DIM "code\n" OFF);
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
    EXPECT_STR_EQ(got, "foo\n" BUL "item");
    free(got);
}

static void test_hard_break_dash_list(void)
{
    char *got = render_one("foo\n- item");
    EXPECT_STR_EQ(got, "foo\n" BUL "item");
    free(got);
}

static void test_hard_break_numbered_list(void)
{
    char *got = render_one("foo\n1. item");
    EXPECT_STR_EQ(got, "foo\n" DIM "1. " OFF "item");
    free(got);
}

static void test_hard_break_heading(void)
{
    /* Next line starting with a heading marker is hard. */
    char *got = render_one("foo\n## h\n");
    EXPECT_STR_EQ(got, "foo\n" BLD "h" OFF "\n");
    free(got);
}

static void test_hard_break_fence(void)
{
    /* Next line starting with ``` opens a fence — must be hard. */
    char *got = render_one("foo\n```\ncode\n```\n");
    EXPECT_STR_EQ(got, "foo\n" DIM "code\n" OFF);
    free(got);
}

static void test_soft_break_carries_bold(void)
{
    /* Bold style continues across a soft break — we don't close on
     * soft \n the way we do on hard. */
    char *got = render_one("**foo\nbar**");
    EXPECT_STR_EQ(got, BLD "foo bar" OFF);
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
     * (CommonMark). The leading spaces are PRESERVED and markers are
     * rendered as dim bullets — `* parent\n  - child` keeps its
     * nesting indent. */
    char *got = render_one("* parent\n  - child\n  - sibling");
    EXPECT_STR_EQ(got, BUL "parent\n  " BUL "child\n  " BUL "sibling");
    free(got);
}

static void test_hard_break_indented_bullet_no_parent(void)
{
    /* Same shape without a containing list — the indent is preserved
     * and markers are prettified. */
    char *got = render_one("intro\n  - one\n  - two");
    EXPECT_STR_EQ(got, "intro\n  " BUL "one\n  " BUL "two");
    free(got);
}

static void test_hard_break_indented_heading(void)
{
    /* `  ## sub` is still a heading. The leading spaces are dropped
     * (CommonMark equivalence) so the heading renders as bold. */
    char *got = render_one("preamble\n  ## sub\nbody");
    EXPECT_STR_EQ(got, "preamble\n" BLD "sub" OFF "\nbody");
    free(got);
}

static void test_hard_break_indented_fence(void)
{
    /* Indented fence opener — same equivalence. */
    char *got = render_one("intro\n  ```\ncode\n```\n");
    EXPECT_STR_EQ(got, "intro\n" DIM "code\n" OFF);
    free(got);
}

static void test_code_fence_indented_closer_closes(void)
{
    /* Fence nested in a list item: every line, the closer included,
     * carries the item's 2-space indent. The closer must still be
     * recognized (CommonMark allows up to 3 spaces) — otherwise the
     * dim region runs past the block to end-of-stream. The opener's
     * indent is normalized away, the body stays verbatim (dim, indent
     * preserved), and the trailing line renders normally. */
    char *got = render_one("  ```\n  code line\n  ```\n  after");
    EXPECT_STR_EQ(got, DIM "  code line\n" OFF "  after");
    free(got);
}

static void test_code_fence_indented_closer_at_eof(void)
{
    /* Indented closer with no trailing newline: md_flush must recognize
     * it (skipping up to 3 leading spaces like the streaming path) and
     * consume it, not emit it as dim content. */
    char *got = render_one("  ```\n  code\n  ```");
    EXPECT_STR_EQ(got, DIM "  code\n" OFF);
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
    EXPECT_STR_EQ(got, BLD "h" OFF "\nbody");
    free(got);
}

static void test_line_start_indented_fence(void)
{
    char *got = render_one("  ```\ncode\n```\n");
    EXPECT_STR_EQ(got, DIM "code\n" OFF);
    free(got);
}

static void test_line_start_indented_thematic(void)
{
    /* Thematic breaks are CommonMark-equivalent under leading spaces —
     * the marker line is normalized and rendered as a dim dinkus. */
    char *got = render_one("  ---\nbody");
    EXPECT_STR_EQ(got, DIM "\xc2\xb7   \xc2\xb7   \xc2\xb7" OFF "\nbody");
    free(got);
}

static void test_line_start_indented_after_blank(void)
{
    /* `intro\n\n  ```\n…` — fence after blank line at line start
     * with leading spaces. Same path. */
    char *got = render_one("intro\n\n  ```\ncode\n```\n");
    EXPECT_STR_EQ(got, "intro\n\n" DIM "code\n" OFF);
    free(got);
}

static void test_line_start_indented_bullet_preserves(void)
{
    /* Bullets are NOT normalized — leading spaces stay so nested
     * lists keep their visual indent even when arriving at line
     * start. The marker is prettified to a dim bullet. */
    char *got = render_one("  - item");
    EXPECT_STR_EQ(got, "  " BUL "item");
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
    /* `---` / `***` / `___` is a thematic break — rendered as a dim
     * dinkus on its own line. */
    char *got = render_one("section\n---\nnext bit");
    EXPECT_STR_EQ(got, "section\n" DIM "\xc2\xb7   \xc2\xb7   \xc2\xb7" OFF "\nnext bit");
    free(got);
}

static void test_hard_break_thematic_stars(void)
{
    char *got = render_one("section\n***\nnext bit");
    EXPECT_STR_EQ(got, "section\n" DIM "\xc2\xb7   \xc2\xb7   \xc2\xb7" OFF "\nnext bit");
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
     * `_ _ _` is a thematic break — rendered as a dim dinkus. */
    char *got = render_one("section\n_ _ _\nnext");
    EXPECT_STR_EQ(got, "section\n" DIM "\xc2\xb7   \xc2\xb7   \xc2\xb7" OFF "\nnext");
    free(got);
}

static void test_hard_break_spaced_thematic_stars(void)
{
    /* `* * *` is recognized as a thematic break (spaced markers with
     * 3+ instances) and rendered as a dim dinkus. */
    char *got = render_one("section\n* * *\nnext");
    EXPECT_STR_EQ(got, "section\n" DIM "\xc2\xb7   \xc2\xb7   \xc2\xb7" OFF "\nnext");
    free(got);
}

static void test_hard_break_thematic_underscores(void)
{
    char *got = render_one("section\n___\nnext bit");
    EXPECT_STR_EQ(got, "section\n" DIM "\xc2\xb7   \xc2\xb7   \xc2\xb7" OFF "\nnext bit");
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
    EXPECT_STR_EQ(got, "intro\n" BUL "alpha\n" BUL "beta");
    free(got);
}

static void test_hard_break_numbered_paren(void)
{
    /* `N)` numbered list marker, alongside `N.`. */
    char *got = render_one("intro\n1) alpha\n2) beta");
    EXPECT_STR_EQ(got, "intro\n" DIM "1) " OFF "alpha\n" DIM "2) " OFF "beta");
    free(got);
}

static void test_hard_break_table_row(void)
{
    /* GFM table — even in no-wrap mode the renderer lays out the aligned
     * grid at natural column widths (wrap_width<=0 means unlimited).
     * Header cells are bold, column separators are dim │, the underline
     * uses ─┼─ crossings, body cells are plain. */
    const char *in = "| Tool | Does |\n|---|---|\n| read | Read files |\n| bash | Run shell |";
    char *got = render_one(in);
    const char *want = BLD "Tool" OFF " " DIM "\xe2\x94\x82" OFF " " BLD "Does" OFF "\n" DIM
                           "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                           "\xe2\x94\xbc"
                           "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                           "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                           "\xe2\x94\x80" OFF "\n"
                           "read " DIM "\xe2\x94\x82" OFF " Read files\n"
                           "bash " DIM "\xe2\x94\x82" OFF " Run shell\n";
    EXPECT_STR_EQ(got, want);
    free(got);
}

static void test_code_fence_no_soft_join(void)
{
    /* Inside a code fence, \n stays \n — fences are verbatim. */
    char *got = render_one("```\nline1\nline2\n```\n");
    EXPECT_STR_EQ(got, DIM "line1\nline2\n" OFF);
    free(got);
}

/* ---------- mixed snippet from real session ---------- */

static void test_real_world_paragraph(void)
{
    /* Compact slice that exercises bold, inline code, em dash, plain text. */
    const char *in = "I'm **hax**, with `read` and `bash` tools.";
    char *got = render_one(in);
    const char *want =
        "I'm " BLD "hax" OFF ", with " CODE "read" CODE_OFF " and " CODE "bash" CODE_OFF " tools.";
    EXPECT_STR_EQ(got, want);
    free(got);
}

/* ---------- wrap mode ---------- */

/* Walk a byte stream as a terminal would, returning the final visible
 * content. Handles the two cursor-control sequences the eager-wrap
 * engine emits — CSI nD (cursor back) and CSI K (erase to EOL) — by
 * tracking a per-row cell map (cell index → byte offset). \n commits
 * the current row; SGR and other CSI escapes pass through verbatim at
 * the row's tail (zero-width, no cell advance). The output is what
 * the user would see on screen, so wrap tests can keep their
 * pre-eager byte-exact expected strings as the invariant.
 *
 * cols selects the right-edge model:
 *   - cols <= 0: unbounded row (rows break only on the engine's own \n).
 *     This is what the logical-wrap tests assert against.
 *   - cols  > 0: physical width with deferred autowrap. A glyph in the
 *     last column sets a "phantom" and leaves the cursor *on* that
 *     column (not past it), exactly as xterm's pending-wrap and
 *     libvterm's at_phantom do; the wrap fires on the next glyph. A
 *     CSI nD while phantom is set therefore lands one cell further left
 *     than a renderer that assumes the cursor sits past the last glyph
 *     — the divergence that ate trailing characters in narrow terminals.
 *
 * Cells assume one codepoint = one cell — adequate for everything the
 * suite exercises (no wide CJK, no combining marks). Continuation
 * bytes (0b10xxxxxx) tag along with the prior cell's byte range. */
static char *interpret_terminal(const char *in, int cols)
{
    struct buf out;
    buf_init(&out);
    size_t in_len = strlen(in);

    /* Generous fixed-size scratch — tests stay well within these. */
    char row[8192];
    size_t row_len = 0;
    size_t cell_off[4096];
    size_t n_cells = 0;
    size_t cursor = 0;
    int phantom = 0;

    size_t i = 0;
    while (i < in_len) {
        unsigned char c = (unsigned char)in[i];

        if (c == '\n') {
            buf_append(&out, row, row_len);
            buf_append(&out, "\n", 1);
            row_len = 0;
            n_cells = 0;
            cursor = 0;
            phantom = 0;
            i++;
            continue;
        }

        if (c == 0x1b && i + 1 < in_len && in[i + 1] == '[') {
            /* Parse CSI: digits up to a final byte in 0x40-0x7E. */
            size_t p = i + 2;
            int n = 0;
            int has_num = 0;
            while (p < in_len && in[p] >= '0' && in[p] <= '9') {
                n = n * 10 + (in[p] - '0');
                has_num = 1;
                p++;
            }
            if (p >= in_len || (unsigned char)in[p] < 0x40 || (unsigned char)in[p] > 0x7E) {
                /* Malformed — treat as literal byte and move on. */
                row[row_len++] = (char)c;
                i++;
                continue;
            }
            char final = in[p];
            if (final == 'D') {
                int back = has_num ? n : 1;
                if (back > (int)cursor)
                    back = (int)cursor;
                cursor -= (size_t)back;
                phantom = 0; /* cursor move resolves the pending wrap */
                i = p + 1;
                continue;
            }
            if (final == 'K') {
                if (cursor < n_cells) {
                    row_len = cell_off[cursor];
                    n_cells = cursor;
                }
                i = p + 1;
                continue;
            }
            /* SGR or any other CSI — pass through. Eager wrap only
             * emits cursor-control sequences when cursor == n_cells,
             * so appending at row_len keeps style escapes adjacent to
             * the cells they were emitted next to. */
            size_t seq_len = p - i + 1;
            memcpy(row + row_len, in + i, seq_len);
            row_len += seq_len;
            i = p + 1;
            continue;
        }

        /* Visible byte. UTF-8 continuation bytes don't bump the cell
         * count — they extend the byte range of the prior cell. */
        int is_continuation = (c & 0xC0) == 0x80;
        if (!is_continuation) {
            /* Deferred autowrap: a glyph held at the phantom column (or
             * past a hard edge) commits the wrap now — flush + \n + new
             * row, then land here. */
            if (cols > 0 && (phantom || (int)cursor >= cols)) {
                buf_append(&out, row, row_len);
                buf_append(&out, "\n", 1);
                row_len = 0;
                n_cells = 0;
                cursor = 0;
                phantom = 0;
            }
            cell_off[n_cells] = row_len;
            n_cells++;
            row[row_len++] = (char)c;
            /* Filling the last column sets the phantom and pins the
             * cursor on it (libvterm leaves pos.col un-advanced); a
             * later CSI nD then counts back from there. */
            if (cols > 0 && (int)n_cells >= cols) {
                phantom = 1;
                cursor = n_cells - 1;
            } else {
                cursor = n_cells;
            }
            i++;
            continue;
        }
        row[row_len++] = (char)c;
        i++;
    }

    buf_append(&out, row, row_len);
    char *s = buf_steal(&out);
    return s ? s : xstrdup("");
}

/* Render with wrap enabled at the given cell budget, then interpret the
 * raw byte stream as a terminal would. The eager-emit engine streams
 * each codepoint to emit_cb immediately and retroactively erases when
 * a word overshoots wrap_width; interpret_terminal collapses that
 * cursor-jiggle back to the visible result so tests assert what users
 * see, not the wire-level chatter. */
static char *render_wrap(const char *input, int wrap_width)
{
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, wrap_width);
    md_feed(m, input, strlen(input));
    md_flush(m);
    md_free(m);
    char *raw = buf_steal(&out);
    char *visible = interpret_terminal(raw ? raw : "", 0);
    free(raw);
    return visible;
}

/* Strip every space and newline so two renderings can be compared for
 * dropped/added glyphs regardless of where the wrap engine placed line
 * breaks (break-spaces are legitimately dropped). */
static char *strip_ws(const char *s)
{
    char *out = xmalloc(strlen(s) + 1);
    size_t j = 0;
    for (const char *p = s; *p; p++)
        if (*p != ' ' && *p != '\n')
            out[j++] = *p;
    out[j] = 0;
    return out;
}

/* Strip ANSI CSI escape sequences (\x1b[ ... final-byte) AND spaces/newlines
 * from a rendered string. Used by test_wrap_phantom_list_reserves_last_column
 * to compare prettified rendered output (which contains dim escapes and the
 * • glyph) against a normalized expected content string. */
static char *strip_ansi_ws(const char *s)
{
    size_t len = strlen(s);
    /* Output is at most len bytes (stripping only removes). */
    char *out = xmalloc(len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\n') {
            i++;
            continue;
        }
        /* ANSI CSI: \x1b[ ... <final byte in 0x40-0x7E> */
        if (c == 0x1b && i + 1 < len && s[i + 1] == '[') {
            size_t p = i + 2;
            while (p < len && (unsigned char)s[p] >= 0x20 && (unsigned char)s[p] < 0x40)
                p++;
            if (p < len && (unsigned char)s[p] >= 0x40 && (unsigned char)s[p] <= 0x7e)
                p++;
            i = p;
            continue;
        }
        out[j++] = (char)c;
        i++;
    }
    out[j] = 0;
    return out;
}

/* Return a whitespace-stripped expected content string for a list item input
 * with its marker normalized to the rendered form:
 *   `- ` / `* ` / `+ ` (possibly with leading spaces) → `•` (U+2022, 3 bytes)
 *   `N. ` / `N) ` (possibly with leading spaces) → digits + `.`/`)` verbatim
 * Everything else passes through; whitespace is then stripped by strip_ws. */
static char *expected_list_content(const char *input)
{
    const char *p = input;
    /* Skip leading spaces. */
    while (*p == ' ')
        p++;
    char buf[256];
    size_t n = 0;
    /* Unordered bullet: replace marker with bullet glyph (no leading spaces). */
    if ((*p == '-' || *p == '*' || *p == '+') && p[1] == ' ') {
        /* bullet glyph U+2022 = \xe2\x80\xa2 */
        buf[n++] = '\xe2';
        buf[n++] = '\x80';
        buf[n++] = '\xa2';
        p += 2; /* skip marker + space */
    } else if (*p >= '0' && *p <= '9') {
        /* Ordered: keep digits + delimiter verbatim. */
        while (*p >= '0' && *p <= '9')
            buf[n++] = *p++;
        if (*p == '.' || *p == ')')
            buf[n++] = *p++;
        if (*p == ' ')
            p++; /* skip the space */
    }
    /* Append the item content. */
    while (*p)
        buf[n++] = *p++;
    buf[n] = 0;
    return strip_ws(buf);
}

/* Render at wrap_width and replay through a physical terminal of the
 * given width (phantom-aware — see interpret_terminal's cols > 0 path). */
static char *render_wrap_phantom(const char *input, int wrap_width, int term_cols)
{
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, wrap_width);
    md_feed(m, input, strlen(input));
    md_flush(m);
    md_free(m);
    char *raw = buf_steal(&out);
    char *visible = interpret_terminal(raw ? raw : "", term_cols);
    free(raw);
    return visible;
}

/* Reserving the last column (wrap_width = term_cols - 1, as md_wrap_width
 * does in production) keeps the eager retro-wrap off the phantom column,
 * so no glyph is dropped on a terminal that defers the autowrap. The
 * range dips below 40 because term_width() no longer floors there — a
 * 20-column sidebar must wrap at 19 and stay intact. */
static void test_wrap_phantom_reserves_last_column(void)
{
    const char *in = "I'm hax, a minimalist coding assistant designed to help you "
                     "with your code and environment management.";
    for (int cols = 20; cols <= 64; cols++) {
        char *vis = render_wrap_phantom(in, cols - 1, cols);
        char *a = strip_ws(vis), *b = strip_ws(in);
        EXPECT_STR_EQ(a, b);
        free(a);
        free(b);
        free(vis);
    }
}

/* Same guarantee for list-item continuation rows. current_row_budget()
 * once floored the budget at indent_cells + 20, which overshot wrap_width
 * on a narrow terminal — a plain "- " bullet (indent 2) wrapped at 22 on a
 * 20-column sidebar, refilling the physical last column the reserved-column
 * scheme is meant to keep clear. Each marker's continuation indent must
 * still wrap one cell inside the edge and lose no glyph.
 *
 * With prettification, vis now contains ANSI escapes and the "•" glyph
 * instead of the literal `-`/`*`/`+` marker, so we compare
 * strip_ansi_ws(vis) against expected_list_content(input): the input's
 * marker is normalized to the rendered form and then whitespace-stripped,
 * giving the same content-glyph set the renderer produced. */
static void test_wrap_phantom_list_reserves_last_column(void)
{
    const char *items[] = {
        "- alpha bravo charlie delta echo foxtrot golf hotel india juliet",
        "1. alpha bravo charlie delta echo foxtrot golf hotel india juliet",
        "  * alpha bravo charlie delta echo foxtrot golf hotel india juliet",
    };
    for (size_t k = 0; k < sizeof(items) / sizeof(items[0]); k++) {
        char *expected = expected_list_content(items[k]);
        for (int cols = 20; cols <= 64; cols++) {
            char *vis = render_wrap_phantom(items[k], cols - 1, cols);
            char *a = strip_ansi_ws(vis);
            EXPECT_STR_EQ(a, expected);
            free(a);
            free(vis);
        }
        free(expected);
    }
}

/* Guard the diagnosis: filling the physical last column (wrap_width ==
 * term_cols) under phantom semantics DOES drop the char before a wrap's
 * break-space — the bug md_wrap_width's reserved column fixes. If a
 * future change makes the eager retro-wrap phantom-safe on its own, this
 * will start passing and can be retired. */
static void test_wrap_phantom_last_column_drops_char(void)
{
    char *vis = render_wrap_phantom("alpha bravo charlie delta echo foxtrot golf", 20, 20);
    char *a = strip_ws(vis), *b = strip_ws("alphabravocharliedeltaechofoxtrotgolf");
    EXPECT(strcmp(a, b) != 0);
    free(a);
    free(b);
    free(vis);
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
     * marker ("• ") is 2 cells, so continuation rows carry a 2-cell
     * hanging indent and wrap at wrap_width (20) like the first row. */
    char *got = render_wrap("* alpha beta gamma delta epsilon zeta", 20);
    EXPECT_STR_EQ(got, BUL "alpha beta gamma\n  delta epsilon zeta");
    free(got);
}

static void test_wrap_numbered_list_indent(void)
{
    /* "1. " marker is 3 cells, so continuation indent is 3. */
    char *got = render_wrap("1. alpha beta gamma delta epsilon", 20);
    EXPECT_STR_EQ(got, DIM "1. " OFF "alpha beta gamma\n   delta epsilon");
    free(got);
}

static void test_wrap_nested_bullet_hanging_indent(void)
{
    /* Nested list: parent at col 0, child indented 2. The child's
     * wrap continuation must indent under its own content column
     * (col 4 = 2 leading spaces + "• ") rather than col 0. The indent
     * fits within wrap_width, so the continuation rows wrap at
     * wrap_width like everything else (current_row_budget) — no
     * overshoot, keeping the reserved last column clear. */
    const char *in = "* parent\n  - child alpha beta gamma delta epsilon zeta";
    char *got = render_wrap(in, 22);
    EXPECT_STR_EQ(got, BUL "parent\n"
                           "  " BUL "child alpha beta\n"
                           "    gamma delta\n"
                           "    epsilon zeta");
    free(got);
}

static void test_wrap_long_numbered_marker_indent(void)
{
    /* 4-digit marker "1000. " is 6 cells. Continuation rows must
     * indent under the content column, not collapse to 0. */
    char *got = render_wrap("1000. alpha beta gamma delta epsilon zeta", 24);
    EXPECT_STR_EQ(got, DIM "1000. " OFF "alpha beta gamma\n      delta epsilon zeta");
    free(got);
}

static void test_wrap_dash_list_indent(void)
{
    char *got = render_wrap("- alpha beta gamma delta epsilon", 20);
    EXPECT_STR_EQ(got, BUL "alpha beta gamma\n  delta epsilon");
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
    const char *want = DIM "this is a deliberately long code line that exceeds the budget\n" OFF;
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
    EXPECT_STR_EQ(got, BLD "alpha beta\ngamma delta" OFF);
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
    EXPECT_STR_EQ(got, "foo " CODE "code with\nspaces" CODE_OFF " bar baz");
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

static void test_wrap_soft_wrapped_list_item(void)
{
    /* Model emitted a list item with an internal soft break (no
     * marker on the continuation line). The renderer must fold the
     * \n into a space and re-flow the joined item at our width with
     * the correct hanging indent under the bullet. */
    char *got = render_wrap("* alpha beta gamma delta\nepsilon zeta eta theta iota", 20);
    EXPECT_STR_EQ(got, BUL "alpha beta gamma\n  delta epsilon zeta\n  eta theta iota");
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
    EXPECT_STR_EQ(got, BUL "list item indented continuation\n" BUL "another item another indent");
    free(got);
}

static void test_wrap_indented_continuation_reflows(void)
{
    /* Same input, wrap enabled at 22 cells: each item joins into a
     * single logical line, then re-flows with hanging indent 2. */
    const char *in = "* list item\n  indented continuation\n* another item\n  another indent";
    char *got = render_wrap(in, 22);
    EXPECT_STR_EQ(got, BUL "list item indented\n"
                           "  continuation\n" BUL "another item another\n"
                           "  indent");
    free(got);
}

static void test_wrap_blank_line_continuation_indents(void)
{
    /* A list-item continuation paragraph separated by a blank line has
     * no marker of its own. Its leading 2-space indent should still set
     * the hanging indent so wrapped rows align under the item instead
     * of collapsing to column 0 (the bug: the wrapped tail landed at
     * col 0). The blank line stays a paragraph break. */
    const char *in =
        "- header\n\n  continuation body long enough to wrap onto a second row here ok";
    char *got = render_wrap(in, 24);
    EXPECT_STR_EQ(got, BUL "header\n"
                           "\n"
                           "  continuation body long\n"
                           "  enough to wrap onto a\n"
                           "  second row here ok");
    free(got);
}

static void test_wrap_code_fence_in_list_item(void)
{
    /* End-to-end of the list-item pathologies: a bulleted item, a blank
     * line, an indented fenced code block (opener/body/closer all carry
     * the item's indent), then a trailing line. The fence must open and
     * close cleanly (dim only around the body) and the trailing line
     * must render normally — not swallowed by a runaway code span. */
    const char *in = "- item\n\n  ```\n  code\n  ```\n  after text";
    char *got = render_wrap(in, 40);
    EXPECT_STR_EQ(got, BUL "item\n"
                           "\n" DIM "  code\n" OFF "  after text");
    free(got);
}

/* ---------- tables ---------- */

static void test_table_buffering_is_feed_split_invariant(void)
{
    /* A table can't be laid out until every row is seen, so it's buffered
     * across feeds. The result must be identical however the bytes are
     * chunked — feed byte-by-byte and compare to the single-feed render. */
    const char *in = "| Name | Age |\n|---|---|\n| Bob | 30 |\n| Alice | 7 |";
    char *whole = render_wrap(in, 40);
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 40);
    for (size_t i = 0; i < strlen(in); i++)
        md_feed(m, in + i, 1);
    md_flush(m);
    md_free(m);
    char *chunked = buf_steal(&out);
    EXPECT_STR_EQ(chunked ? chunked : "", whole);
    free(chunked);
    free(whole);
}

static void test_table_cell_inline_styles(void)
{
    /* Cells run through the inline engine, so `**bold**` and `` `code` ``
     * render styled. Crucially the column width is measured on the visible
     * glyphs (the Note column is 6 = "bold x", not 10 = "**bold** x"), so
     * the `a`/`bb` cells stay correctly padded — the width tally excludes
     * the markers and SGR escapes. */
    const char *in = "| Col | Note |\n|---|---|\n| a | **bold** x |\n| bb | `code` |";
    char *got = render_wrap(in, 40);
    EXPECT_STR_EQ(got, BLD "Col" OFF TSEP BLD "Note" OFF
                           "\n" DIM HL HL HL HL CR HL HL HL HL HL HL HL OFF "\n"
                           "a  " TSEP BLD "bold" OFF " x\n"
                           "bb " TSEP CODE "code" CODE_OFF "\n");
    free(got);
}

static void test_table_cell_block_markers_stay_literal(void)
{
    /* GFM table cells are inline contexts, so leading block markers in a
     * cell (`#`, `-`, `---`) are NOT interpreted as a heading / bullet /
     * rule — they render as literal inline text. Header cells are bold;
     * the body `---` cell stays "---", not a dim divider. Cols: "# H"=3,
     * "- x"=3 (the "1"/"a" bodies are narrower). */
    const char *in = "| # H | - x |\n|---|---|\n| --- | a |";
    char *got = render_wrap(in, 40);
    EXPECT_STR_EQ(got, BLD "# H" OFF TSEP BLD "- x" OFF "\n" DIM HL HL HL HL CR HL HL HL HL OFF "\n"
                           "---" TSEP "a\n");
    free(got);
}

static void test_table_header_cell_inline_bold_stays_bold(void)
{
    /* A header cell with an inner **bold** span stays fully bold: the
     * inner span's bold-off must not cancel the header bold for the rest
     * of the cell. "A **B** C" renders as bold "A B C" (suppress_bold
     * drops the cell's own toggles; the outer header bold covers it). */
    const char *in = "| A **B** C | V |\n|---|---|\n| 1 | 2 |";
    char *got = render_wrap(in, 40);
    EXPECT_STR_EQ(got, BLD "A B C" OFF TSEP BLD "V" OFF "\n" DIM HL HL HL HL HL HL CR HL HL OFF "\n"
                           "1    " TSEP "2\n");
    free(got);
}

static void test_table_header_only_eof_no_newline(void)
{
    /* A header + delimiter with no trailing newline stays deferred until
     * md_flush resolves the complete pair at EOF. */
    const char *in = "| A | B |\n|---|---|";
    char *got = render_wrap(in, 40);
    EXPECT_STR_EQ(got, BLD "A" OFF TSEP BLD "B" OFF "\n" DIM HL HL CR HL HL OFF "\n");
    free(got);
}

static void test_table_long_streamed_row_no_leak(void)
{
    /* A body row longer than the parser tail cap must remain whole while
     * table collection is active, with no prose emitted ahead of it. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 40);
    md_feed(m, "| H |\n|---|\n", 12);
    char *row = xmalloc(9000);
    row[0] = '|';
    row[1] = ' ';
    memset(row + 2, 'a', 8990);
    row[8992] = ' ';
    row[8993] = '|';
    md_feed(m, row, 8994); /* 9 KB row, no newline yet */
    md_feed(m, "\n", 1);
    md_flush(m);
    md_free(m);
    char *got = buf_steal(&out);
    /* Output begins with the rendered table (reflow bullet's dim), not
     * leaked 'aaaa' prose, and the cell content survives. */
    EXPECT(got && strncmp(got, DIM, strlen(DIM)) == 0);
    EXPECT(got && strstr(got, "aaaaaaaaaaaaaaaaaaaa") != NULL);
    free(got);
    free(row);
}

static void test_table_oversized_inprogress_row_bails(void)
{
    /* A newline-less row beyond the table cap must bail the buffered block
     * to verbatim output and leave collection mode. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 40);
    md_feed(m, "| H |\n|---|\n", 12);
    size_t big = 70000; /* > TABLE_MAX_BYTES (64 KiB) */
    char *row = xmalloc(big);
    row[0] = '|';
    row[1] = ' ';
    memset(row + 2, 'a', big - 4);
    row[big - 2] = ' ';
    row[big - 1] = '|';
    md_feed(m, row, big); /* no newline — would otherwise buffer unbounded */
    md_feed(m, "\n", 1);
    md_flush(m);
    md_free(m);
    char *got = buf_steal(&out);
    EXPECT(got && strstr(got, "| H |") != NULL); /* verbatim header, not a formatted grid */
    free(got);
    free(row);
}

static void test_table_oversized_complete_row_passes_through(void)
{
    /* A complete row rejected by table collection must fall through the parser without loss. */
    size_t big = 70000; /* > TABLE_MAX_BYTES (64 KiB) */
    char *in = xmalloc(big + 64);
    int p = snprintf(in, big + 64, "| H |\n|---|\n| ");
    memset(in + p, 'a', big);
    p += (int)big;
    p += snprintf(in + p, 64, " |\n");

    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 40);
    md_feed(m, in, (size_t)p);
    md_flush(m);
    md_free(m);
    char *got = buf_steal(&out);
    EXPECT(got && strncmp(got, BLD, strlen(BLD)) == 0);
    EXPECT(got && strstr(got, "| aaaaaaaaaaaaaaaaaaaa") != NULL);

    free(got);
    free(in);
}

static void test_table_oversized_header_passes_through(void)
{
    /* A header rejected by the table probe must remain ordinary parser output. */
    size_t big = 70000; /* > TABLE_MAX_BYTES (64 KiB) */
    char *in = xmalloc(big + 64);
    int p = snprintf(in, big + 64, "| ");
    memset(in + p, 'H', big);
    p += (int)big;
    p += snprintf(in + p, 64, " |\n|---|\n| x |\n");

    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 40);
    md_feed(m, in, (size_t)p);
    md_flush(m);
    md_free(m);
    char *got = buf_steal(&out);
    EXPECT(got && strncmp(got, "| HHHHHHHHHHHHHHHHHHHH", 22) == 0);
    EXPECT(got && strstr(got, "| x |\n") != NULL);

    free(got);
    free(in);
}

static void test_table_eof_over_cap_row_passes_through(void)
{
    /* A final row left in the tail by table finish must still be emitted at flush. */
    size_t row = 60000;
    size_t last = 10000;
    char *in = xmalloc(row + last + 64);
    int p = snprintf(in, 64, "| H |\n|---|\n| ");
    memset(in + p, 'a', row);
    p += (int)row;
    p += snprintf(in + p, 16, " |\n| ");
    memset(in + p, 'b', last);
    p += (int)last;
    p += snprintf(in + p, 16, " |");

    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 40);
    md_feed(m, in, (size_t)p);
    md_flush(m);
    md_free(m);
    char *got = buf_steal(&out);
    EXPECT(got && strstr(got, "| bbbbbbbbbbbbbbbbbbbb") != NULL);

    free(got);
    free(in);
}

static void test_table_reflow_value_keeps_bold_across_wrap(void)
{
    /* A bold cell span that wraps must re-assert bold from the table replay
     * into the wrapper's continuation row. */
    const char *in = "| K | V |\n|---|---|\n| k | **alpha beta**gammazz delta |";
    char *got = render_wrap(in, 18);
    char *vis = interpret_terminal(got, 0);
    EXPECT(strstr(vis, BLD "beta" OFF) != NULL);
    free(vis);
    free(got);
}

/* ---------- thematic break (dinkus) ---------- */

static void test_dinkus_renders_three_dots(void)
{
    char *got = render_wrap("a\n\n---\n\nb", 40);
    EXPECT_STR_EQ(got, "a\n\n" DINKUS "\n\nb");
    free(got);
}

static void test_dinkus_shrinks_on_narrow_width(void)
{
    /* Three dots + two 3-space gaps need 9 cells; at width 5 it drops to
     * two dots (1 + 3 + 1 = 5) so the divider never wraps. */
    char *got = render_wrap("a\n\n***\n\nb", 5);
    EXPECT_STR_EQ(got, "a\n\n" DIM DOT "   " DOT OFF "\n\nb");
    free(got);
}

static void test_wrap_no_autowrap_when_sgr_after_held_space(void)
{
    /* Regression for the autowrap variant flagged in code review: a
     * Markdown style marker (** / ` / _) arriving right at the edge
     * mustn't trigger autowrap. With pending_wrap the edge-space is
     * dropped (no byte at col > wrap_width), and a zero-width raw
     * that follows is emitted to the wire without committing the
     * pending wrap — that lets a subsequent visible byte commit on
     * the new row, and the SGR's position relative to \n is moot
     * (SGR state persists across \n on standard terminals).
     *
     * "foo bar **baz**" at width 7: edge-space sets pending; bold
     * opener emits inline (no commit); 'b' commits → \n + indent;
     * "baz" lands on row 2; bold closer emits inline. Wire stream
     * has no byte at col > 7. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 7);
    md_feed(m, "foo bar **baz**", 15);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "foo bar" BLD "\nbaz" OFF);
    free(s);
}

static void test_wrap_sgr_during_pending_wrap_no_extra_blank_line(void)
{
    /* Regression: wrap_append used to commit pending_wrap on any
     * byte, including zero-width SGR escapes. When a styled span
     * had its trailing edge-space at col > wrap_width followed by
     * the style closer + hard \n, the closer would commit pending
     * (emitting an early \n + the closer on its own row), and the
     * hard \n + blank-line \n then produced TWO blank rows instead
     * of one paragraph break.
     *
     * Source `` `foo bar `\n\nnext `` at width 7: cyan opens, "foo
     * bar" fills col 7, the trailing space pends, cyan closes (now
     * inline, no commit), hard \n absorbs pending, second \n is
     * the paragraph break, "next" on a fresh row. interpret_terminal
     * preserves SGR escapes inline, so the visible-row stream still
     * carries the cyan markers — what matters here is the row count:
     * one blank line between the styled "foo bar" and "next", not
     * two as in the buggy case. */
    char *got = render_wrap("`foo bar `\n\nnext", 7);
    EXPECT_STR_EQ(got, CODE "foo bar" CODE_OFF "\n\nnext");
    free(got);
}

static void test_wrap_sgr_during_pending_wrap_at_eof(void)
{
    /* EOF variant of the same bug: a styled span ending at the wrap
     * edge with the closer immediately after the dropped space must
     * not commit pending. md_flush leaves pending unresolved (no
     * trailing \n is owed), so the wire stays a single styled run.
     *
     * Source `` `foo bar ` `` at width 7: cyan + "foo bar" fills
     * col 7, trailing space pends, cyan closes inline. No commit.
     * Wire: cyan-on + "foo bar" + cyan-off. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 7);
    md_feed(m, "`foo bar `", 10);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, CODE "foo bar" CODE_OFF);
    free(s);
}

static void test_wrap_replay_restores_sgr_state_at_break(void)
{
    /* Regression: with eager SGR emit, a style transition inside the
     * erased slice has already mutated terminal state before wrap_break
     * fires. The replayed partial word would inherit that *post-break*
     * state instead of the state at the break point, rendering
     * unstyled text where the source had it styled (or vice versa).
     *
     * Fix: snapshot in_bold / in_italic / in_inline_code at every
     * break-space record. At wrap_break, emit a diff so terminal state
     * reverts to the snapshot before the replay re-applies its own
     * SGRs.
     *
     * Input "**foo bar**baz" at width 8: wrap fires at 'a' of "baz"
     * (the only break candidate is the space between "foo" and
     * "bar"). The bold-close \e[22m was eagerly emitted on row 1
     * before CSI nD; without the rewind, replayed "bar" would render
     * plain even though it's inside the bold span at the break. The
     * rewind emits \e[1m on row 2 before the replay, so "bar" comes
     * out bold; the replay's own \e[22m then turns it off in time
     * for "baz". */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 8);
    md_feed(m, "**foo bar**baz", 14);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, BLD "foo bar" OFF "b" CUB(5) ERASE "\n" BLD "bar" OFF "baz");
    free(s);
}

static void test_wrap_eof_preserves_trailing_space_when_within_budget(void)
{
    /* The autowrap drop in wrap_flush_all only kicks in when the held
     * space sits past the wrap edge. When the row hasn't reached the
     * edge, the trailing space is emitted as usual — inline-code
     * spans are documented as verbatim, so `` `x ` `` (with a
     * trailing space inside the cyan span) must keep its space at
     * end-of-stream. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 80);
    md_feed(m, "`x `", 4);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, CODE "x " CODE_OFF);
    free(s);
}

static void test_wrap_hard_newline_drops_trailing_held_space(void)
{
    /* Regression: a held break-space at col > wrap_width would autowrap
     * if flushed at hard \n. wrap_flush_all drops it instead — trailing
     * spaces before a hard break are invisible, and emitting one at
     * the right edge commits xterm's delayed-wrap before the \n fires,
     * leaving a stranded blank row.
     *
     * "foo bar \n\nnext" at width 7: row 1 fills to col 7 with
     * "foo bar". The trailing space sits at col 8 (held). Hard \n
     * fires. With the drop, the wire stream is "foo bar\n\nnext" —
     * no trailing space, no autowrap, one blank line as intended.
     * Without the drop, the wire would be "foo bar \n\nnext" and
     * xterm would autowrap the space, producing two blank lines. */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 7);
    md_feed(m, "foo bar \n\nnext", 14);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "foo bar\n\nnext");
    free(s);
}

static void test_wrap_sgr_after_break_space_preserves_source_order(void)
{
    /* When content does follow the held space + deferred SGR (no
     * wrap), flush_held_space emits them in source order: space first,
     * then the SGR, then the content. The cyan-on lands between the
     * space and the marker's content — matching what a non-eager
     * renderer would produce. */
    char *got = render_wrap("foo `bar` baz", 80);
    EXPECT_STR_EQ(got, "foo " CODE "bar" CODE_OFF " baz");
    free(got);
}

static void test_wrap_no_escapes_when_break_at_budget_edge(void)
{
    /* Regression for the terminal-autowrap bug: when wrap_width matches
     * the terminal width and a break-space would land at col == budget +
     * 1, eagerly emitting it commits the terminal's delayed autowrap to
     * the next physical row before wrap_break can erase. CSI nD can't
     * cross row boundaries, so the erase + \n leaves a stranded blank
     * row.
     *
     * Fix: hold every break-space until the next byte arrives. If the
     * next byte is content that fits, flush the held space first. If
     * it's content that overflows, wrap_break drops the held space
     * without emitting it — no cursor-back needed (nothing on the
     * terminal past the prior committed content).
     *
     * "foo bar baz" at width 7: the space after "bar" would land at
     * col 8. Held. 'b' triggers wrap_break, which sees space_held=1
     * and skips the cursor-back+erase entirely. Wire stream contains
     * no cursor escapes — just "foo bar\nbaz". */
    struct buf out;
    buf_init(&out);
    struct md_renderer *m = md_new(capture, &out, 7);
    md_feed(m, "foo bar baz", 11);
    md_flush(m);
    md_free(m);
    char *s = buf_steal(&out);
    EXPECT_STR_EQ(s, "foo bar\nbaz");
    free(s);
}

static void test_wrap_hard_break_at_budget_no_extra_line(void)
{
    /* Trailing hard-break spaces landing exactly on the wrap budget:
     * the first space sets pending_wrap, the second is dropped (not
     * committed), and the hard-break \n then subsumes pending — a
     * single break, not "aaaaa\n \nbar" with a stray space line. */
    char *got = render_wrap("aaaaa  \nbar", 5);
    EXPECT_STR_EQ(got, "aaaaa\nbar");
    free(got);
}

static void test_wrap_heading_not_wrapped(void)
{
    /* Headings are single-line by policy — long heading content
     * passes through verbatim even when wrap is active. */
    char *got = render_wrap("## A very long heading that exceeds the budget\n", 20);
    EXPECT_STR_EQ(got, BLD "A very long heading that exceeds the budget" OFF "\n");
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
    test_single_blank_line_preserved();
    test_multiple_blank_lines_collapse();
    test_leading_blank_lines_stripped();
    test_blank_lines_preserved_in_code_fence();
    test_collapse_blank_lines_split_across_feeds();

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
    test_bullet_collapses_padded_marker();
    test_numbered_collapses_padded_marker();
    test_padded_marker_indent_preserved();
    test_padded_marker_split_across_feeds();
    test_numbered_marker_only_at_eof();
    test_bullet_marker_only_at_eof_stays_literal();
    test_bare_marker_no_space_stays_literal();

    test_split_double_star();
    test_split_inline_code();
    test_split_fence_at_line_start();
    test_split_heading();

    test_feed_partition_invariance();
    test_feed_emits_prose_eagerly();

    test_flush_emits_unterminated_marker();
    test_flush_emits_pending_tail();
    test_flush_closes_italic_at_eof();
    test_flush_closes_underscore_italic_at_eof();
    test_flush_closes_bold_at_eof();
    test_flush_closes_fence_at_eof();

    test_reset_clears_state();

    test_emit_kind_separates_content_and_sgr();
    test_emit_kind_marks_wrap_cursor_control_raw();
    test_emit_kind_covers_table_paths();

    test_styled_mode_transition_soft_resets();
    test_in_table_tracks_buffering();

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
    test_eof_bare_thematic_no_trailing_newline();
    test_crlf_thematic_break();
    test_eof_soft_join_digits();
    test_eof_soft_join_dash_alone();
    test_eof_soft_join_hash_alone();
    test_eof_hard_break_trailing_spaces_ambiguous_prefix();
    test_hard_break_crlf();
    test_eof_hard_break_blank();
    test_soft_break_simple_join();
    test_soft_break_skip_leading_whitespace();
    test_soft_break_no_double_space();
    test_hard_break_trailing_two_spaces();
    test_hard_break_trailing_two_spaces_after_code_span();
    test_hard_break_trailing_spaces_split_across_feeds();
    test_soft_break_single_trailing_space();
    test_hard_break_carries_emphasis();
    test_trailing_spaces_before_delimiter_soft_join();
    test_hard_break_before_block_closes_emphasis();
    test_hard_break_before_fence_closes_emphasis();
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
    test_code_fence_indented_closer_closes();
    test_code_fence_indented_closer_at_eof();
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
    test_wrap_long_word_overflow();
    test_wrap_phantom_reserves_last_column();
    test_wrap_phantom_list_reserves_last_column();
    test_wrap_phantom_last_column_drops_char();
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
    test_wrap_soft_wrapped_list_item();
    test_soft_join_indented_continuation();
    test_wrap_indented_continuation_reflows();
    test_wrap_blank_line_continuation_indents();
    test_wrap_code_fence_in_list_item();

    test_table_buffering_is_feed_split_invariant();
    test_table_cell_inline_styles();
    test_table_cell_block_markers_stay_literal();
    test_table_header_cell_inline_bold_stays_bold();
    test_table_header_only_eof_no_newline();
    test_table_long_streamed_row_no_leak();
    test_table_oversized_inprogress_row_bails();
    test_table_oversized_complete_row_passes_through();
    test_table_oversized_header_passes_through();
    test_table_eof_over_cap_row_passes_through();
    test_table_reflow_value_keeps_bold_across_wrap();
    test_dinkus_renders_three_dots();
    test_dinkus_shrinks_on_narrow_width();

    test_wrap_no_escapes_when_break_at_budget_edge();
    test_wrap_no_autowrap_when_sgr_after_held_space();
    test_wrap_sgr_during_pending_wrap_no_extra_blank_line();
    test_wrap_sgr_during_pending_wrap_at_eof();
    test_wrap_sgr_after_break_space_preserves_source_order();
    test_wrap_replay_restores_sgr_state_at_break();
    test_wrap_hard_newline_drops_trailing_held_space();
    test_wrap_eof_preserves_trailing_space_when_within_budget();
    test_wrap_hard_break_at_budget_no_extra_line();
    test_wrap_heading_not_wrapped();

    T_REPORT();
}
