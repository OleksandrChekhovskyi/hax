/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "harness.h"
#include "text/completion.h"

static void add_all(struct completion *completion, const char *const *candidates)
{
    for (; *candidates; candidates++)
        completion_add(completion, *candidates);
}

static void expect_extension(const char *const *candidates, const char *word, const char *expected)
{
    struct completion completion = {0};
    add_all(&completion, candidates);
    completion_keep_prefixed(&completion, word);
    char *extended = completion_extend(&completion, word);

    if (!expected)
        EXPECT(extended == NULL);
    else if (!extended)
        FAIL("no extension of '%s', expected '%s'", word, expected);
    else
        EXPECT_STR_EQ(extended, expected);
    free(extended);
    completion_free(&completion);
}

static void test_extend_like_a_shell(void)
{
    const char *const words[] = {"model", "help", "preset", "preset-save", "clear", "copy", NULL};

    expect_extension(words, "mo", "model ");
    expect_extension(words, "help", "help ");
    expect_extension(words, "pre", "preset");
    expect_extension(words, "preset-", "preset-save ");
    expect_extension(words, "preset", NULL);
    expect_extension(words, "c", NULL);
    expect_extension(words, "", NULL);
    expect_extension(words, "zzz", NULL);
}

static void test_extend_keeps_utf8_whole(void)
{
    /* é and ê share their first byte. */
    const char *const accented[] = {"éclair", "êclair", NULL};
    expect_extension(accented, "", NULL);

    const char *const shared[] = {"café-noir", "café-crème", NULL};
    expect_extension(shared, "c", "café-");
}

static void test_keep_prefixed_preserves_order(void)
{
    const char *const words[] = {"focus", "review", "fast", "f", NULL};
    struct completion completion = {0};
    add_all(&completion, words);

    completion_keep_prefixed(&completion, "");
    EXPECT(completion.count == 4);

    completion_keep_prefixed(&completion, "f");
    EXPECT(completion.count == 3);
    if (completion.count == 3) {
        EXPECT_STR_EQ(completion.candidates[0], "focus");
        EXPECT_STR_EQ(completion.candidates[1], "fast");
        EXPECT_STR_EQ(completion.candidates[2], "f");
    }

    completion_free(&completion);
    EXPECT(completion.count == 0 && completion.candidates == NULL);
}

static void test_sort_orders_bytewise(void)
{
    struct completion completion = {0};
    completion_sort(&completion);
    EXPECT(completion.count == 0);

    const char *const words[] = {"review", "Fast", "focus", NULL};
    add_all(&completion, words);
    completion_sort(&completion);
    EXPECT_STR_EQ(completion.candidates[0], "Fast");
    EXPECT_STR_EQ(completion.candidates[1], "focus");
    EXPECT_STR_EQ(completion.candidates[2], "review");
    completion_free(&completion);
}

int main(void)
{
    test_extend_like_a_shell();
    test_extend_keeps_utf8_whole();
    test_keep_prefixed_preserves_order();
    test_sort_orders_bytewise();
    T_REPORT();
}
