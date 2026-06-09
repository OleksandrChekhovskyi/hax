/* SPDX-License-Identifier: MIT */
#include "harness.h"
#include "terminal/picker.h"

/* picker_run itself needs a tty and can't be driven headless; the
 * filter predicate it scrolls over is the pure, testable piece. */

static void test_empty_query_matches_all(void)
{
    EXPECT(picker_match("anything", ""));
    EXPECT(picker_match("anything", NULL));
    EXPECT(picker_match("", ""));
    /* A NULL label matches only the empty query. */
    EXPECT(picker_match(NULL, ""));
    EXPECT(!picker_match(NULL, "x"));
}

static void test_substring_case_insensitive(void)
{
    EXPECT(picker_match("anthropic/claude-opus-4", "opus"));
    EXPECT(picker_match("anthropic/claude-opus-4", "OPUS"));
    EXPECT(picker_match("anthropic/claude-opus-4", "Claude"));
    EXPECT(picker_match("anthropic/claude-opus-4", "4"));
    EXPECT(!picker_match("anthropic/claude-opus-4", "gpt"));
}

static void test_all_terms_must_match(void)
{
    /* Space-separated terms are AND-ed, order-independent. */
    EXPECT(picker_match("anthropic/claude-opus-4", "anthropic opus"));
    EXPECT(picker_match("anthropic/claude-opus-4", "opus anthropic"));
    EXPECT(!picker_match("anthropic/claude-opus-4", "anthropic gpt"));
}

static void test_whitespace_handling(void)
{
    /* Leading / trailing / repeated spaces yield no spurious empty terms. */
    EXPECT(picker_match("hello world", "  hello   world  "));
    EXPECT(picker_match("hello world", "   "));
    /* The query's spaces are term separators, not literal — so a single
     * token that isn't itself present fails even if its pieces appear. */
    EXPECT(!picker_match("hello world", "llowo"));
}

int main(void)
{
    test_empty_query_matches_all();
    test_substring_case_insensitive();
    test_all_terms_must_match();
    test_whitespace_handling();
    T_REPORT();
}
