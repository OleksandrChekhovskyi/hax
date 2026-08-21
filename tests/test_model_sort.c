/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "model_sort.h"

static int order_qsort(const void *left, const void *right)
{
    return model_id_order(*(const char *const *)left, *(const char *const *)right);
}

#define EXPECT_ORDER(first, second)                                                                \
    do {                                                                                           \
        EXPECT(model_id_order(first, second) < 0);                                                 \
        EXPECT(model_id_order(second, first) > 0);                                                 \
    } while (0)

static void expect_sorted(const char **scrambled, const char *const *expected, size_t count)
{
    const char *sorted[32];
    EXPECT(count <= sizeof(sorted) / sizeof(sorted[0]));
    memcpy(sorted, scrambled, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), order_qsort);
    for (size_t i = 0; i < count; i++)
        EXPECT_STR_EQ(sorted[i], expected[i]);
}

static void test_version_before_base_before_snapshot_before_variant(void)
{
    EXPECT_ORDER("gpt-5.6", "gpt-5");
    EXPECT_ORDER("gpt-5", "gpt-5-2025-08-07");
    EXPECT_ORDER("gpt-5-2025-08-07", "gpt-5-mini");
    EXPECT_ORDER("gpt-5.6", "gpt-5.6-2026-10-20");
    EXPECT_ORDER("gpt-5-mini", "gpt-5-mini-2025-08-07");
}

static void test_version_schemas_order_alike(void)
{
    /* "x-y" minors, "x.y" minors, and glued "namex.y" all beat the shorter version. */
    EXPECT_ORDER("claude-opus-4-8", "claude-opus-4-7");
    EXPECT_ORDER("claude-opus-5", "claude-opus-4-8");
    EXPECT_ORDER("claude-opus-4-5", "claude-opus-4");
    EXPECT_ORDER("kimi-k2.7-code", "kimi-k2.6");
    EXPECT_ORDER("qwen3.6-plus", "qwen3.5-plus");
    EXPECT_ORDER("minimax-m3", "minimax-m2.7");
}

static void test_numeric_not_lexicographic(void)
{
    EXPECT_ORDER("gpt-5.10", "gpt-5.9");
    EXPECT_ORDER("babbage-002", "babbage-001");
    EXPECT_ORDER("gpt-4o-2024-11-20", "gpt-4o-2024-08-06");
}

static void test_versions_beat_words_and_dates(void)
{
    EXPECT_ORDER("grok-4.6", "grok-build-0.1");
    EXPECT_ORDER("text-embedding-3-large", "text-embedding-ada-002");
    /* A version continuation outranks a snapshot of the shorter id. */
    EXPECT_ORDER("gpt-5.4", "gpt-5-2025-08-07");
    /* Four or more digits read as a date even without separators. */
    EXPECT_ORDER("tts-1", "tts-1-1106");
    EXPECT_ORDER("tts-1-1106", "tts-1-hd");
    EXPECT_ORDER("claude-opus-4-5", "claude-opus-4-5-20251101");
}

static void test_families_group_case_insensitively(void)
{
    EXPECT_ORDER("GLM-5", "glm-4.6");
    EXPECT_ORDER("deepseek-v4-pro", "GPT-5");
}

static void test_total_order_tie_break(void)
{
    EXPECT(model_id_order("gpt-4.1", "gpt-4.1") == 0);
    /* Identical token streams still order deterministically, by raw bytes. */
    EXPECT_ORDER("gpt-4-1", "gpt-4.1");
    EXPECT(model_id_order("", "") == 0);
    EXPECT_ORDER("", "gpt-5");
}

static void test_openai_style_list(void)
{
    const char *scrambled[] = {
        "gpt-4",       "o3-mini",     "gpt-5-mini",   "gpt-4.1", "gpt-5.4",          "gpt-4o",
        "gpt-5",       "gpt-4-turbo", "gpt-5.4-pro",  "o4-mini", "gpt-5-2025-08-07", "gpt-4o-mini",
        "gpt-5-codex", "o3",          "gpt-4.1-nano",
    };
    const char *const expected[] = {
        "gpt-5.4", "gpt-5.4-pro",  "gpt-5",   "gpt-5-2025-08-07", "gpt-5-codex", "gpt-5-mini",
        "gpt-4.1", "gpt-4.1-nano", "gpt-4",   "gpt-4o",           "gpt-4o-mini", "gpt-4-turbo",
        "o4-mini", "o3",           "o3-mini",
    };
    expect_sorted(scrambled, expected, sizeof(expected) / sizeof(expected[0]));
}

static void test_anthropic_style_list(void)
{
    const char *scrambled[] = {
        "claude-opus-5",
        "claude-sonnet-5",
        "claude-fable-5",
        "claude-opus-4-8",
        "claude-opus-4-7",
        "claude-sonnet-4-6",
        "claude-opus-4-6",
        "claude-opus-4-5-20251101",
        "claude-haiku-4-5-20251001",
        "claude-sonnet-4-5-20250929",
    };
    const char *const expected[] = {
        "claude-fable-5",
        "claude-haiku-4-5-20251001",
        "claude-opus-5",
        "claude-opus-4-8",
        "claude-opus-4-7",
        "claude-opus-4-6",
        "claude-opus-4-5-20251101",
        "claude-sonnet-5",
        "claude-sonnet-4-6",
        "claude-sonnet-4-5-20250929",
    };
    expect_sorted(scrambled, expected, sizeof(expected) / sizeof(expected[0]));
}

static void test_gateway_style_list(void)
{
    const char *scrambled[] = {
        "qwen3.5-plus",
        "minimax-m2.7",
        "glm-5",
        "deepseek-v4-flash-free",
        "kimi-k3",
        "glm-5.2",
        "deepseek-v4-pro",
        "minimax-m3",
        "qwen3.6-plus",
        "kimi-k2.7-code",
        "deepseek-v4-flash",
        "glm-5.1",
        "hy3-preview",
        "hy3",
        "grok-build-0.1",
        "grok-4.6",
    };
    const char *const expected[] = {
        "deepseek-v4-flash",
        "deepseek-v4-flash-free",
        "deepseek-v4-pro",
        "glm-5.2",
        "glm-5.1",
        "glm-5",
        "grok-4.6",
        "grok-build-0.1",
        "hy3",
        "hy3-preview",
        "kimi-k3",
        "kimi-k2.7-code",
        "minimax-m3",
        "minimax-m2.7",
        "qwen3.6-plus",
        "qwen3.5-plus",
    };
    expect_sorted(scrambled, expected, sizeof(expected) / sizeof(expected[0]));
}

int main(void)
{
    test_version_before_base_before_snapshot_before_variant();
    test_version_schemas_order_alike();
    test_numeric_not_lexicographic();
    test_versions_beat_words_and_dates();
    test_families_group_case_insensitively();
    test_total_order_tie_break();
    test_openai_style_list();
    test_anthropic_style_list();
    test_gateway_style_list();
    T_REPORT();
}
