/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "text/frontmatter.h"

/* A NULL want expects no value. */
static void expect_description(const char *content, const char *want)
{
    char *value = frontmatter_scalar_line(content, strlen(content), "description");
    if (!want) {
        if (value)
            FAIL("want no value, got \"%s\"", value);
    } else if (!value) {
        FAIL("want \"%s\", got no value", want);
    } else {
        EXPECT_STR_EQ(value, want);
    }
    free(value);
}

static void test_plain(void)
{
    expect_description("---\ndescription: does X\n---\n", "does X");
    expect_description("---\ndescription:\tdoes X  \n---\n", "does X");
    expect_description("---\nname: x\ndescription: does X\nlicense: MIT\n---\nbody\n", "does X");
    /* Some third-party skills leave `: ` in unquoted prose. */
    expect_description("---\ndescription: Build for AWS: ECS\n---\n", "Build for AWS: ECS");
}

static void test_plain_multiline(void)
{
    expect_description("---\n"
                       "description: Auto-detect biggest inflections across all\n"
                       "  metrics\n"
                       "name: inflection\n"
                       "---\n",
                       "Auto-detect biggest inflections across all metrics");
    expect_description("---\ndescription:\n  starts on\n\n  the next line\n---\n",
                       "starts on the next line");
}

static void test_comments(void)
{
    expect_description("---\ndescription: does X # note\n---\n", "does X");
    expect_description("---\ndescription: C# and F#\n---\n", "C# and F#");
    expect_description("---\ndescription: # note\n  does X\n---\n", "does X");
    expect_description("---\ndescription: does X\n# note\n  not part of it\n---\n", "does X");
    expect_description("---\ndescription: \"does X\" # note\n---\n", "does X");
}

static void test_single_quoted(void)
{
    expect_description("---\ndescription: 'it''s # not a comment'\n---\n", "it's # not a comment");
    expect_description("---\ndescription: 'wrapped\n  across lines'\n---\n",
                       "wrapped across lines");
    expect_description("---\ndescription: 'unterminated\n---\n", NULL);
}

static void test_double_quoted(void)
{
    expect_description("---\ndescription: \"does X\"\n---\n", "does X");
    expect_description("---\ndescription:\n"
                       "  \"Solve competition math problems with adversarial\n"
                       "  verification. Activates when asked to 'solve this IMO problem'.\"\n"
                       "---\n",
                       "Solve competition math problems with adversarial verification. "
                       "Activates when asked to 'solve this IMO problem'.");
    expect_description("---\ndescription: \"say \\\"hi\\\" \\\\ caf\\u00e9 \\U0001F600\"\n---\n",
                       "say \"hi\" \\ caf\xc3\xa9 \xf0\x9f\x98\x80");
    expect_description("---\ndescription: \"tab\\tand\\nnewline\"\n---\n", "tab and newline");
    expect_description("---\ndescription: \"a\\vb\\fc\\Nd\\Le\\Pf\"\n---\n", "a b c d e f");
    expect_description("---\ndescription: \"x\\a\\b\\ey\\_z\"\n---\n", "x\a\b\x1By\xC2\xA0z");
    expect_description("---\ndescription: \"joined \\\n    lines, no\\\n  space\"\n---\n",
                       "joined lines, nospace");
    /* Unknown, incomplete, or non-scalar escapes stay verbatim. */
    expect_description("---\ndescription: \"\\q \\u12 \\uD800\"\n---\n", "\\q \\u12 \\uD800");
    expect_description("---\ndescription: \"unterminated\n---\n", NULL);
    expect_description("---\ndescription: \"trailing backslash\\\n---\n", NULL);
}

static void test_block(void)
{
    expect_description("---\n"
                       "description: >\n"
                       "  Multiline skills description with leading folded block scalar.\n"
                       "  Use this skill for this and that.\n"
                       "name: example\n"
                       "---\n",
                       "Multiline skills description with leading folded block scalar. "
                       "Use this skill for this and that.");
    expect_description("---\ndescription: |\n  first\n\n    indented\n  last\n---\n",
                       "first indented last");
    expect_description("---\ndescription: >-\n  stripped\n---\n", "stripped");
    expect_description("---\ndescription: |2+ # note\n  kept\n\n---\n", "kept");
    expect_description("---\ndescription: |\n  # not a comment\n---\n", "# not a comment");
    expect_description("---\ndescription: >\n---\n", NULL);
    expect_description("---\ndescription: >text\n  more\n---\n", NULL);
    /* Quoted indicators are ordinary text. */
    expect_description("---\ndescription: \"|\"\n---\n", "|");
}

static void test_key_matching(void)
{
    expect_description("---\nname: x\n---\n", NULL);
    expect_description("---\ndescription:\n---\n", NULL);
    expect_description("---\ndescription: \"\"\n---\n", NULL);
    expect_description("---\nmetadata:\n  description: nested\n---\n", NULL);
    expect_description("---\ndescriptions: other key\n---\n", NULL);
    expect_description("---\ndescription:glued\n---\n", NULL);
    expect_description("---\ndescription: first\ndescription: second\n---\n", "first");
}

static void test_fences(void)
{
    expect_description("description: no fence\n", NULL);
    expect_description("\n---\ndescription: late fence\n---\n", NULL);
    expect_description("---\ndescription: unterminated\n", NULL);
    expect_description("---\ndescription: >\n  unterminated\n", NULL);
    expect_description("---\ndescription: no final newline\n---", "no final newline");
    expect_description("---\r\ndescription: crlf\r\n  wrapped\r\n---\r\nbody\r\n", "crlf wrapped");
    expect_description("---\r\ndescription: \"joined \\\r\n  crlf\"\r\n---\r\n", "joined crlf");
}

static void test_line_breaks_fold(void)
{
    expect_description("---\ndescription: \"a\\u2028b\\x85c\\u2029d\\x0Be\\x0Cf\"\n---\n",
                       "a b c d e f");
    expect_description("---\ndescription: a\xE2\x80\xA8"
                       "b\xC2\x85"
                       "c\xE2\x80\xA9"
                       "d\ve\ff\n---\n",
                       "a b c d e f");
    /* Other codepoints sharing those lead bytes are text. */
    expect_description("---\ndescription: a\xE2\x80\x94"
                       "b\xC2\xA0"
                       "c\n---\n",
                       "a\xE2\x80\x94"
                       "b\xC2\xA0"
                       "c");
}

static void test_sanitized(void)
{
    const char content[] = "---\ndescription: a\0b\xFF\n---\n";
    char *value = frontmatter_scalar_line(content, sizeof(content) - 1, "description");
    EXPECT(value != NULL);
    if (value)
        EXPECT_STR_EQ(value, "a\xEF\xBF\xBD"
                             "b\xEF\xBF\xBD");
    free(value);

    /* A decoded escape is sanitized like a raw byte. */
    expect_description("---\ndescription: \"A\\0B \\x00\"\n---\n", "A\xEF\xBF\xBD"
                                                                   "B \xEF\xBF\xBD");
}

int main(void)
{
    test_plain();
    test_plain_multiline();
    test_comments();
    test_single_quoted();
    test_double_quoted();
    test_block();
    test_key_matching();
    test_fences();
    test_line_breaks_fold();
    test_sanitized();

    T_REPORT();
}
