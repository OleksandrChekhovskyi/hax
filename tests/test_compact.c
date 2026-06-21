/* SPDX-License-Identifier: MIT */
#include "compact.h"
#include "config.h"
#include "harness.h"

/* compact_over_threshold is the pure trigger predicate; everything else
 * (compact_should_auto, agent_compact) layers config + I/O on top of it. */
static void test_over_threshold(void)
{
    /* Unknown window (limit <= 0) or unreported ctx never triggers. */
    EXPECT(!compact_over_threshold(100, 0, 85));
    EXPECT(!compact_over_threshold(100, -1, 85));
    EXPECT(!compact_over_threshold(-1, 1000, 85));

    /* Below / at / above the percentage boundary. */
    EXPECT(!compact_over_threshold(8499, 10000, 85));
    EXPECT(compact_over_threshold(8500, 10000, 85));
    EXPECT(compact_over_threshold(9999, 10000, 85));

    /* 100% threshold only fires when fully at the window. */
    EXPECT(!compact_over_threshold(9999, 10000, 100));
    EXPECT(compact_over_threshold(10000, 10000, 100));
}

static void test_should_auto(void)
{
    /* Drive the config tiers via runtime overrides so the test is
     * hermetic regardless of env / file config. */
    config_set_override("compact.auto", "1");
    config_set_override("compact.threshold", "90");

    EXPECT(!compact_should_auto(8999, 10000)); /* 89.99% < 90% */
    EXPECT(compact_should_auto(9000, 10000));  /* exactly 90% */
    EXPECT(!compact_should_auto(9999, 0));     /* unknown window */

    /* Disabled via config: never auto-compacts, even when far over. */
    config_set_override("compact.auto", "0");
    EXPECT(!compact_should_auto(100000, 10000));

    /* Out-of-range threshold falls back to the 85% default. */
    config_set_override("compact.auto", "1");
    config_set_override("compact.threshold", "0");
    EXPECT(!compact_should_auto(8499, 10000));
    EXPECT(compact_should_auto(8500, 10000));
}

int main(void)
{
    test_over_threshold();
    test_should_auto();
    T_REPORT();
}
