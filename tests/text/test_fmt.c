/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "text/fmt.h"

static void test_parse_int(void)
{
    int value = 0;
    EXPECT(parse_int("42", &value));
    EXPECT(value == 42);

    char text[64];
    snprintf(text, sizeof(text), "%d", INT_MIN);
    EXPECT(parse_int(text, &value));
    EXPECT(value == INT_MIN);
    snprintf(text, sizeof(text), "%d", INT_MAX);
    EXPECT(parse_int(text, &value));
    EXPECT(value == INT_MAX);

    value = 7;
    EXPECT(!parse_int(NULL, &value));
    EXPECT(!parse_int("", &value));
    EXPECT(!parse_int("12x", &value));
    EXPECT(!parse_int("999999999999999999999", &value));
    EXPECT(value == 7);
}

static void test_parse_hex(void)
{
    uint32_t value = 0;
    EXPECT(parse_hex("0", 1, &value));
    EXPECT(value == 0);
    EXPECT(parse_hex("aF", 2, &value));
    EXPECT(value == 0xAF);
    EXPECT(parse_hex("FFFFFFFF", 8, &value));
    EXPECT(value == 0xFFFFFFFF);
    /* Only count digits are read, so a digit past the end is ignored. */
    EXPECT(parse_hex("123", 2, &value));
    EXPECT(value == 0x12);

    value = 7;
    EXPECT(!parse_hex("1g", 2, &value));
    EXPECT(!parse_hex("-1", 2, &value));
    EXPECT(!parse_hex("", 0, &value));
    EXPECT(!parse_hex("123456789", 9, &value));
    EXPECT(value == 7);
}

static void test_format_duration_ranges(void)
{
    char buf[32];
    format_duration(buf, sizeof(buf), 0);
    EXPECT_STR_EQ(buf, "0s");
    format_duration(buf, sizeof(buf), -5); /* clamps, never "-0s" */
    EXPECT_STR_EQ(buf, "0s");
    format_duration(buf, sizeof(buf), 42499); /* rounds down */
    EXPECT_STR_EQ(buf, "42s");
    format_duration(buf, sizeof(buf), 42500); /* rounds up */
    EXPECT_STR_EQ(buf, "43s");
    format_duration(buf, sizeof(buf), 68000);
    EXPECT_STR_EQ(buf, "1m 08s");
    format_duration(buf, sizeof(buf), 3720000);
    EXPECT_STR_EQ(buf, "1h 02m");
    /* Zero remainders are omitted: whole minutes and hours read bare. */
    format_duration(buf, sizeof(buf), 600000);
    EXPECT_STR_EQ(buf, "10m");
    format_duration(buf, sizeof(buf), 7200000);
    EXPECT_STR_EQ(buf, "2h");
    /* The steady variant keeps them, so ticking displays never shrink. */
    format_duration_steady(buf, sizeof(buf), 600000);
    EXPECT_STR_EQ(buf, "10m 00s");
    format_duration_steady(buf, sizeof(buf), 7200000);
    EXPECT_STR_EQ(buf, "2h 00m");
    format_duration_steady(buf, sizeof(buf), 68000);
    EXPECT_STR_EQ(buf, "1m 08s");
}

static void test_format_duration_extreme(void)
{
    char formatted[64];
    format_duration(formatted, sizeof(formatted), LONG_MAX);
    EXPECT(formatted[0] != '-');
    EXPECT(strchr(formatted, 'h') != NULL);
}

static void test_format_cost_precision(void)
{
    char buf[32];
    format_cost(buf, sizeof(buf), 0.0);
    EXPECT_STR_EQ(buf, "$0.00");
    format_cost(buf, sizeof(buf), 0.00421);
    EXPECT_STR_EQ(buf, "$0.0042");
    format_cost(buf, sizeof(buf), 0.042);
    EXPECT_STR_EQ(buf, "$0.042");
    format_cost(buf, sizeof(buf), 1.234);
    EXPECT_STR_EQ(buf, "$1.23");
    format_cost(buf, sizeof(buf), 42.129);
    EXPECT_STR_EQ(buf, "$42.13");
}

static void test_format_tokens_ranges(void)
{
    char buf[32];
    format_tokens(buf, sizeof(buf), -1);
    EXPECT_STR_EQ(buf, "?");
    format_tokens(buf, sizeof(buf), 412);
    EXPECT_STR_EQ(buf, "412");
    format_tokens(buf, sizeof(buf), 5410);
    EXPECT_STR_EQ(buf, "5.4k");
    format_tokens(buf, sizeof(buf), 2000); /* whole multiples print bare */
    EXPECT_STR_EQ(buf, "2k");
    format_tokens(buf, sizeof(buf), 262144); /* decimal suffixes even for binary windows */
    EXPECT_STR_EQ(buf, "262k");
    format_tokens(buf, sizeof(buf), 872000);
    EXPECT_STR_EQ(buf, "872k");
    format_tokens(buf, sizeof(buf), 1000000);
    EXPECT_STR_EQ(buf, "1M");
    format_tokens(buf, sizeof(buf), 1200000);
    EXPECT_STR_EQ(buf, "1.2M");
    format_tokens(buf, sizeof(buf), 12000000);
    EXPECT_STR_EQ(buf, "12M");
}

static void test_format_context_with_and_without_limit(void)
{
    char buf[64];
    format_context(buf, sizeof(buf), 9113, 262144);
    EXPECT_STR_EQ(buf, "9.1k / 262k (3%)");
    format_context(buf, sizeof(buf), 9113, 0); /* unknown window */
    EXPECT_STR_EQ(buf, "9.1k");
    format_context(buf, sizeof(buf), 300000, 262144); /* stale window metadata reports over 100% */
    EXPECT_STR_EQ(buf, "300k / 262k (114%)");
    format_context(buf, sizeof(buf), -1, 262144); /* known window, no usage reported yet */
    EXPECT_STR_EQ(buf, "? / 262k");
    format_context(buf, sizeof(buf), -1, 0); /* nothing known */
    EXPECT_STR_EQ(buf, "?");
}

static void test_format_usage_extremes(void)
{
    char formatted[64];
    format_tokens(formatted, sizeof(formatted), LONG_MAX);
    EXPECT(formatted[0] != '-');

    format_context(formatted, sizeof(formatted), LONG_MAX, 1);
    EXPECT(strstr(formatted, "(999%)") != NULL);
}

int main(void)
{
    test_parse_int();
    test_parse_hex();

    test_format_duration_ranges();
    test_format_duration_extreme();
    test_format_cost_precision();
    test_format_tokens_ranges();
    test_format_context_with_and_without_limit();
    test_format_usage_extremes();

    T_REPORT();
}
