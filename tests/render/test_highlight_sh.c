/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "harness.h"
#include "render/highlight_sh.h"

#define MAX_SPANS 16

struct span_cap {
    const char *text[MAX_SPANS];
    size_t len[MAX_SPANS];
    enum sh_span_kind kind[MAX_SPANS];
    int n;
};

static void cap_span(const char *bytes, size_t n, enum sh_span_kind kind, void *user)
{
    struct span_cap *c = user;
    if (c->n < MAX_SPANS) {
        c->text[c->n] = bytes;
        c->len[c->n] = n;
        c->kind[c->n] = kind;
        c->n++;
    }
}

static struct span_cap classify(const char *s)
{
    struct span_cap c = {0};
    highlight_sh(s, strlen(s), cap_span, &c);
    return c;
}

static void expect_span(const struct span_cap *c, int i, enum sh_span_kind kind, const char *text)
{
    EXPECT(c->n > i);
    if (c->n <= i)
        return;
    EXPECT(c->kind[i] == kind);
    EXPECT_MEM_EQ(c->text[i], c->len[i], text, strlen(text));
}

static void test_plain_and_flags_stay_plain(void)
{
    /* Flags, paths, and words carry no meaning the classifier knows; they stay one run. */
    struct span_cap c = classify("ls -la /tmp");
    EXPECT(c.n == 1);
    expect_span(&c, 0, SH_SPAN_PLAIN, "ls -la /tmp");
}

static void test_single_quotes(void)
{
    struct span_cap c = classify("echo 'hello world'");
    EXPECT(c.n == 2);
    expect_span(&c, 0, SH_SPAN_PLAIN, "echo ");
    expect_span(&c, 1, SH_SPAN_STRING, "'hello world'");
}

static void test_double_quotes(void)
{
    struct span_cap c = classify("echo \"a b\" done");
    EXPECT(c.n == 3);
    expect_span(&c, 0, SH_SPAN_PLAIN, "echo ");
    expect_span(&c, 1, SH_SPAN_STRING, "\"a b\"");
    expect_span(&c, 2, SH_SPAN_PLAIN, " done");
}

static void test_comment_to_end_of_line(void)
{
    struct span_cap c = classify("make test # run tests");
    EXPECT(c.n == 2);
    expect_span(&c, 0, SH_SPAN_PLAIN, "make test ");
    expect_span(&c, 1, SH_SPAN_COMMENT, "# run tests");
}

static void test_hash_inside_word_is_plain(void)
{
    struct span_cap c = classify("echo a#b");
    EXPECT(c.n == 1);
    expect_span(&c, 0, SH_SPAN_PLAIN, "echo a#b");
}

static void test_comment_after_separator(void)
{
    struct span_cap c = classify("a;#c");
    EXPECT(c.n == 3);
    expect_span(&c, 0, SH_SPAN_PLAIN, "a");
    expect_span(&c, 1, SH_SPAN_OPERATOR, ";");
    expect_span(&c, 2, SH_SPAN_COMMENT, "#c");
}

static void test_operators_grouped(void)
{
    struct span_cap c = classify("a && b || c; d | e");
    EXPECT(c.n == 9);
    expect_span(&c, 0, SH_SPAN_PLAIN, "a ");
    expect_span(&c, 1, SH_SPAN_OPERATOR, "&&");
    expect_span(&c, 2, SH_SPAN_PLAIN, " b ");
    expect_span(&c, 3, SH_SPAN_OPERATOR, "||");
    expect_span(&c, 4, SH_SPAN_PLAIN, " c");
    expect_span(&c, 5, SH_SPAN_OPERATOR, ";");
    expect_span(&c, 6, SH_SPAN_PLAIN, " d ");
    expect_span(&c, 7, SH_SPAN_OPERATOR, "|");
    expect_span(&c, 8, SH_SPAN_PLAIN, " e");
}

static void test_redirect_splits_words(void)
{
    struct span_cap c = classify("make 2>&1 | tee log");
    EXPECT(c.n == 5);
    expect_span(&c, 0, SH_SPAN_PLAIN, "make 2");
    expect_span(&c, 1, SH_SPAN_OPERATOR, ">&");
    expect_span(&c, 2, SH_SPAN_PLAIN, "1 ");
    expect_span(&c, 3, SH_SPAN_OPERATOR, "|");
    expect_span(&c, 4, SH_SPAN_PLAIN, " tee log");
}

static void test_escaped_space_stays_plain(void)
{
    struct span_cap c = classify("echo foo\\ bar");
    EXPECT(c.n == 1);
    expect_span(&c, 0, SH_SPAN_PLAIN, "echo foo\\ bar");
}

static void test_unclosed_quote_colors_to_end(void)
{
    struct span_cap c = classify("echo 'oops");
    EXPECT(c.n == 2);
    expect_span(&c, 0, SH_SPAN_PLAIN, "echo ");
    expect_span(&c, 1, SH_SPAN_STRING, "'oops");
}

static void test_escaped_quote_inside_double_quotes(void)
{
    struct span_cap c = classify("echo \"a\\\"b\"");
    EXPECT(c.n == 2);
    expect_span(&c, 0, SH_SPAN_PLAIN, "echo ");
    expect_span(&c, 1, SH_SPAN_STRING, "\"a\\\"b\"");
}

static void test_quote_chars_nest_literally(void)
{
    struct span_cap c = classify("echo \"it's\" 'a\"b'");
    EXPECT(c.n == 4);
    expect_span(&c, 0, SH_SPAN_PLAIN, "echo ");
    expect_span(&c, 1, SH_SPAN_STRING, "\"it's\"");
    expect_span(&c, 2, SH_SPAN_PLAIN, " ");
    expect_span(&c, 3, SH_SPAN_STRING, "'a\"b'");
}

static void test_quote_state_survives_newline(void)
{
    /* Reflowed header rows are one logical line; the quote spans the break. */
    struct span_cap c = classify("echo 'a\nb'");
    EXPECT(c.n == 2);
    expect_span(&c, 0, SH_SPAN_PLAIN, "echo ");
    expect_span(&c, 1, SH_SPAN_STRING, "'a\nb'");
}

static void test_comment_ends_at_newline(void)
{
    struct span_cap c = classify("# one\n# two");
    EXPECT(c.n == 3);
    expect_span(&c, 0, SH_SPAN_COMMENT, "# one");
    expect_span(&c, 1, SH_SPAN_PLAIN, "\n");
    expect_span(&c, 2, SH_SPAN_COMMENT, "# two");
}

static void test_empty_input_emits_nothing(void)
{
    struct span_cap c = classify("");
    EXPECT(c.n == 0);
}

static void test_lang_names(void)
{
    EXPECT(highlight_sh_lang("sh", 2));
    EXPECT(highlight_sh_lang("bash", 4));
    EXPECT(highlight_sh_lang("shell", 5));
    EXPECT(highlight_sh_lang("BASH", 4));
    EXPECT(highlight_sh_lang("  sh  ", 6));
    EXPECT(highlight_sh_lang("bash strict", 11));
    EXPECT(!highlight_sh_lang("", 0));
    EXPECT(!highlight_sh_lang("python", 6));
    EXPECT(!highlight_sh_lang("shellsession", 12));
    EXPECT(!highlight_sh_lang("c", 1));
}

int main(void)
{
    test_plain_and_flags_stay_plain();
    test_single_quotes();
    test_double_quotes();
    test_comment_to_end_of_line();
    test_hash_inside_word_is_plain();
    test_comment_after_separator();
    test_operators_grouped();
    test_redirect_splits_words();
    test_escaped_space_stays_plain();
    test_unclosed_quote_colors_to_end();
    test_escaped_quote_inside_double_quotes();
    test_quote_chars_nest_literally();
    test_quote_state_survives_newline();
    test_comment_ends_at_newline();
    test_empty_input_emits_nothing();
    test_lang_names();
    T_REPORT();
}
