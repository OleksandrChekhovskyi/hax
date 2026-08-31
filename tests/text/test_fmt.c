/* SPDX-License-Identifier: MIT */
#include <limits.h>
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

int main(void)
{
    test_parse_int();

    test_format_duration_ranges();
    test_format_duration_extreme();
    test_format_cost_precision();

    T_REPORT();
}
