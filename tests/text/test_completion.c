/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "harness.h"
#include "text/completion.h"

static void add_all(struct completion *completion, const char *const *candidates)
{
    for (; *candidates; candidates++)
        completion_add(completion, *candidates);
}

static void expect_split_extension(const char *const *candidates, char separator, const char *word,
                                   const char *expected)
{
    struct completion completion = {.separator = separator};
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

static void expect_extension(const char *const *candidates, const char *word, const char *expected)
{
    expect_split_extension(candidates, '\0', word, expected);
}

static void expect_parts(const char *const *candidates, char separator, const char *word,
                         const char *const *expected)
{
    struct completion completion = {.separator = separator};
    add_all(&completion, candidates);
    completion_keep_prefixed(&completion, word);
    completion_to_parts(&completion, word);

    size_t expected_count = 0;
    while (expected[expected_count])
        expected_count++;
    EXPECT(completion.count == expected_count);
    for (size_t i = 0; i < completion.count && i < expected_count; i++)
        EXPECT_STR_EQ(completion.candidates[i], expected[i]);
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

static void test_extend_stops_past_separator(void)
{
    const char *const keys[] = {"bash.timeout", "bash.timeout_max", "bash.background_yield",
                                "compact.auto", "keep_awake",       NULL};

    expect_split_extension(keys, '.', "ba", "bash.");
    expect_split_extension(keys, '.', "bash.", NULL);
    expect_split_extension(keys, '.', "bash.t", "bash.timeout");
    expect_split_extension(keys, '.', "co", "compact.");
    expect_split_extension(keys, '.', "compact.", "compact.auto ");
    expect_split_extension(keys, '.', "k", "keep_awake ");
    /* Without a separator, the same candidates extend straight through the dot. */
    expect_extension(keys, "co", "compact.auto ");
}

static void test_parts_list_each_part_once(void)
{
    const char *const ids[] = {"openai/gpt-5", "anthropic/claude", "openai/gpt-5-mini", "openai/o3",
                               NULL};

    expect_parts(ids, '/', "", (const char *const[]){"openai/", "anthropic/", NULL});
    expect_parts(ids, '/', "op", (const char *const[]){"openai/", NULL});
    expect_parts(ids, '/', "openai/", (const char *const[]){"gpt-5", "gpt-5-mini", "o3", NULL});
    expect_parts(ids, '/', "openai/gpt", (const char *const[]){"gpt-5", "gpt-5-mini", NULL});
    expect_parts(ids, '\0', "openai/",
                 (const char *const[]){"openai/gpt-5", "openai/gpt-5-mini", "openai/o3", NULL});
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
    test_extend_stops_past_separator();
    test_parts_list_each_part_once();
    test_keep_prefixed_preserves_order();
    test_sort_orders_bytewise();
    T_REPORT();
}
