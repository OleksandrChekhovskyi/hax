/* SPDX-License-Identifier: MIT */
#include <stdio.h>

#include "effort.h"
#include "harness.h"

static void test_effort_set(void)
{
    struct effort_set levels = {0};
    EXPECT(!levels.known);
    EXPECT(!effort_set_has(&levels, "low"));

    EXPECT(effort_set_add(&levels, "low") == 1);
    EXPECT(levels.known && levels.count == 1 && effort_set_has(&levels, "low"));
    EXPECT(effort_set_add(&levels, "low") == 0);
    EXPECT(levels.count == 1);

    struct effort_set empty = {0};
    EXPECT(effort_set_add(&empty, "") == 0);
    EXPECT(empty.known && empty.count == 0);

    EXPECT(effort_set_add(&levels, "an-absurdly-long-effort-name") == 0);
    EXPECT(levels.count == 1);
    for (int i = 0; i < EFFORT_MAX_LEVELS + 3; i++) {
        char value[8];
        snprintf(value, sizeof(value), "l%d", i);
        effort_set_add(&levels, value);
    }
    EXPECT(levels.count == EFFORT_MAX_LEVELS);
}

static void test_effort_clamp(void)
{
    struct effort_set levels = {0};
    effort_set_add(&levels, "low");
    effort_set_add(&levels, "high");

    EXPECT_STR_EQ(effort_clamp(&levels, "high"), "high");
    EXPECT_STR_EQ(effort_clamp(&levels, "xhigh"), "high");
    EXPECT_STR_EQ(effort_clamp(&levels, "medium"), "low");
    EXPECT_STR_EQ(effort_clamp(&levels, "none"), "low");
    EXPECT(effort_clamp(&levels, "ludicrous") == NULL);

    struct effort_set empty = {.known = 1};
    EXPECT(effort_clamp(&empty, "low") == NULL);
    struct effort_set unknown = {0};
    EXPECT(effort_clamp(&unknown, "low") == NULL);
}

int main(void)
{
    test_effort_set();
    test_effort_clamp();
    T_REPORT();
}
