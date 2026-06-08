/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "text/html_markdown.h"

static char *md(const char *html)
{
    return html_to_markdown(html, strlen(html), NULL);
}

static void test_plain_text_passthrough(void)
{
    char *out = md("hello world");
    EXPECT_STR_EQ(out, "hello world");
    free(out);
}

static void test_tags_stripped(void)
{
    char *out = md("<b>bold</b> and <i>italic</i>");
    EXPECT_STR_EQ(out, "bold and italic");
    free(out);
}

static void test_whitespace_collapsed(void)
{
    char *out = md("a   \n\t  b");
    EXPECT_STR_EQ(out, "a b");
    free(out);
}

static void test_headings(void)
{
    char *out = md("<h1>Title</h1><h2>Sub</h2>");
    EXPECT_STR_EQ(out, "# Title\n\n## Sub");
    free(out);
}

static void test_list_items(void)
{
    char *out = md("<ul><li>one</li><li>two</li></ul>");
    EXPECT_STR_EQ(out, "- one\n- two");
    free(out);
}

static void test_paragraph_breaks(void)
{
    char *out = md("<p>first</p><p>second</p>");
    EXPECT_STR_EQ(out, "first\n\nsecond");
    free(out);
}

static void test_anchor_to_markdown_link(void)
{
    char *out = md("see <a href=\"https://x.com/p\">the page</a> now");
    EXPECT_STR_EQ(out, "see [the page](https://x.com/p) now");
    free(out);
}

static void test_anchor_without_href(void)
{
    char *out = md("<a>nolink</a>");
    EXPECT_STR_EQ(out, "nolink");
    free(out);
}

static void test_empty_anchor_dropped(void)
{
    /* A bare jump anchor (<a name="x"></a>) has href but no text — it must
     * not render as an empty "[](#x)" link. */
    char *out = md("<h2><a href=\"#NAME\"></a>NAME</h2>");
    EXPECT_STR_EQ(out, "## NAME");
    free(out);
}

static void test_anchor_javascript_dropped(void)
{
    char *out = md("<a href=\"javascript:void(0)\">x</a>");
    EXPECT_STR_EQ(out, "x");
    free(out);
}

static void test_script_content_dropped(void)
{
    char *out = md("before<script>var x = 1 < 2;</script>after");
    EXPECT_STR_EQ(out, "beforeafter");
    free(out);
}

static void test_style_content_dropped(void)
{
    char *out = md("<style>body { color: red; }</style>text");
    EXPECT_STR_EQ(out, "text");
    free(out);
}

static void test_comment_dropped(void)
{
    char *out = md("a<!-- this is > a comment -->b");
    EXPECT_STR_EQ(out, "ab");
    free(out);
}

static void test_named_entities(void)
{
    char *out = md("a &amp; b &lt; c &gt; d &quot;e&quot;");
    EXPECT_STR_EQ(out, "a & b < c > d \"e\"");
    free(out);
}

static void test_nbsp_collapses(void)
{
    char *out = md("a&nbsp;&nbsp;b");
    EXPECT_STR_EQ(out, "a b");
    free(out);
}

static void test_numeric_entities(void)
{
    char *out = md("&#65;&#x42;&#8212;");
    EXPECT_STR_EQ(out, "AB\xE2\x80\x94");
    free(out);
}

static void test_bare_ampersand_literal(void)
{
    char *out = md("Tom & Jerry");
    EXPECT_STR_EQ(out, "Tom & Jerry");
    free(out);
}

static void test_attribute_with_gt_not_ending_tag(void)
{
    /* The '>' inside the title attribute must not prematurely end the tag. */
    char *out = md("<a href=\"/x\" title=\"a > b\">link</a>");
    EXPECT_STR_EQ(out, "[link](/x)");
    free(out);
}

static void test_pre_preserves_whitespace(void)
{
    char *out = md("<pre>line1\n  line2</pre>");
    EXPECT_STR_EQ(out, "```\nline1\n  line2\n```");
    free(out);
}

static void test_br_newline(void)
{
    char *out = md("a<br>b");
    EXPECT_STR_EQ(out, "a\nb");
    free(out);
}

static void test_collapses_excess_blank_lines(void)
{
    char *out = md("<p>a</p><p></p><p></p><p>b</p>");
    EXPECT_STR_EQ(out, "a\n\nb");
    free(out);
}

static void test_leading_trailing_trimmed(void)
{
    char *out = md("  <p>  hello  </p>  ");
    EXPECT_STR_EQ(out, "hello");
    free(out);
}

static void test_empty_input(void)
{
    char *out = md("");
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_unterminated_tag(void)
{
    char *out = md("text <div incomplete");
    EXPECT_STR_EQ(out, "text");
    free(out);
}

/* A paragraph comfortably over MAIN_MIN_CHARS (200) so main-content scoping is
 * accepted without tripping the whole-document fallback. */
#define LONG_BODY                                                                                  \
    "This is a sufficiently long paragraph of real article content that comfortably exceeds "      \
    "the two hundred character minimum, so the main-content region is accepted on its own "        \
    "without the converter falling back to the whole document."

static void test_main_region_scopes_out_chrome(void)
{
    char *out = md("<nav><a href=\"/x\">Home</a></nav>"
                   "<main><h1>Title</h1><p>" LONG_BODY "</p></main>"
                   "<footer>FOOTER CHROME</footer>");
    EXPECT(strstr(out, "# Title") != NULL);
    EXPECT(strstr(out, "FOOTER CHROME") == NULL); /* outside <main> */
    EXPECT(strstr(out, "Home") == NULL);          /* nav dropped */
    free(out);
}

static void test_role_main_scoped(void)
{
    char *out = md("<div role=\"main\"><p>" LONG_BODY "</p></div>"
                   "<div>SIDEBAR CHROME</div>");
    EXPECT(strstr(out, "article content") != NULL);
    EXPECT(strstr(out, "SIDEBAR CHROME") == NULL);
    free(out);
}

static void test_article_scoped(void)
{
    char *out = md("<header>SITE HEADER</header>"
                   "<article><p>" LONG_BODY "</p></article>");
    EXPECT(strstr(out, "article content") != NULL);
    EXPECT(strstr(out, "SITE HEADER") == NULL);
    free(out);
}

static void test_no_container_converts_whole_page(void)
{
    char *out = md("<div><p>just a div, no semantic container</p></div>");
    EXPECT_STR_EQ(out, "just a div, no semantic container");
    free(out);
}

static void test_nav_and_aside_stripped(void)
{
    char *out = md("<p>keep</p><nav>drop nav</nav><aside>drop aside</aside><p>more</p>");
    EXPECT_STR_EQ(out, "keep\n\nmore");
    free(out);
}

static void test_short_main_falls_back_to_whole_page(void)
{
    /* <main> holds almost nothing, so the real content outside it must still
     * survive via the fallback. */
    char *out = md("<main>x</main><p>" LONG_BODY "</p>");
    EXPECT(strstr(out, "article content") != NULL);
    free(out);
}

static void test_definition_list_not_mashed(void)
{
    /* The classic pathology: a status-code <dl> must not collapse into one
     * run — each term and description gets its own line. */
    char *out = md("<dl><dt>404 Not Found</dt><dd>The resource is missing.</dd>"
                   "<dt>418 Teapot</dt><dd>Short and stout.</dd></dl>");
    EXPECT_STR_EQ(out, "404 Not Found\nThe resource is missing.\n418 Teapot\nShort and stout.");
    free(out);
}

static void test_table_cells_separated(void)
{
    char *out = md("<table><tr><th>Code</th><th>Meaning</th></tr>"
                   "<tr><td>404</td><td>Not Found</td></tr>"
                   "<tr><td>500</td><td>Server Error</td></tr></table>");
    EXPECT_STR_EQ(out, "Code | Meaning\n404 | Not Found\n500 | Server Error");
    free(out);
}

static void test_empty_table_cell(void)
{
    char *out = md("<table><tr><td>a</td><td></td><td>c</td></tr></table>");
    EXPECT_STR_EQ(out, "a | | c");
    free(out);
}

int main(void)
{
    test_plain_text_passthrough();
    test_tags_stripped();
    test_whitespace_collapsed();
    test_headings();
    test_list_items();
    test_paragraph_breaks();
    test_anchor_to_markdown_link();
    test_anchor_without_href();
    test_empty_anchor_dropped();
    test_anchor_javascript_dropped();
    test_script_content_dropped();
    test_style_content_dropped();
    test_comment_dropped();
    test_named_entities();
    test_nbsp_collapses();
    test_numeric_entities();
    test_bare_ampersand_literal();
    test_attribute_with_gt_not_ending_tag();
    test_pre_preserves_whitespace();
    test_br_newline();
    test_collapses_excess_blank_lines();
    test_leading_trailing_trimmed();
    test_empty_input();
    test_unterminated_tag();
    test_main_region_scopes_out_chrome();
    test_role_main_scoped();
    test_article_scoped();
    test_no_container_converts_whole_page();
    test_nav_and_aside_stripped();
    test_short_main_falls_back_to_whole_page();
    test_definition_list_not_mashed();
    test_table_cells_separated();
    test_empty_table_cell();
    T_REPORT();
}
