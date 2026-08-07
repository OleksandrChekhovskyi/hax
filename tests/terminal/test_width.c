/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "harness.h"
#include "terminal/width.h"

static void test_auto_display_width(void)
{
    EXPECT(auto_display_width(19) == 20);
    EXPECT(auto_display_width(20) == 20);
    EXPECT(auto_display_width(100) == 100);
    EXPECT(auto_display_width(101) == 101);
    EXPECT(auto_display_width(110) == 110);
    EXPECT(auto_display_width(111) == 100);
}

static void test_display_width_auto(void)
{
    unsetenv("HAX_DISPLAY_WIDTH");
    int expected = auto_display_width(term_width());
    EXPECT(display_width() == expected);

    setenv("HAX_DISPLAY_WIDTH", "auto", 1);
    EXPECT(display_width() == expected);
    unsetenv("HAX_DISPLAY_WIDTH");
}

static void test_display_width_env_override(void)
{
    int terminal = term_width();
    if (terminal < 20)
        terminal = 20;
    setenv("HAX_DISPLAY_WIDTH", "terminal", 1);
    EXPECT(display_width() == terminal);
    setenv("HAX_DISPLAY_WIDTH", "TERMINAL", 1);
    EXPECT(display_width() == terminal);

    /* An exact width bypasses both terminal detection and the soft cap. */
    setenv("HAX_DISPLAY_WIDTH", "120", 1);
    EXPECT(display_width() == 120);
    setenv("HAX_DISPLAY_WIDTH", "60", 1);
    EXPECT(display_width() == 60);
    setenv("HAX_DISPLAY_WIDTH", "500", 1);
    EXPECT(display_width() == 500);

    int automatic = auto_display_width(term_width());
    /* Out-of-range, malformed, and overflowing values fall back to auto. */
    setenv("HAX_DISPLAY_WIDTH", "5", 1);
    EXPECT(display_width() == automatic);
    setenv("HAX_DISPLAY_WIDTH", "abc", 1);
    EXPECT(display_width() == automatic);
    setenv("HAX_DISPLAY_WIDTH", "80x", 1);
    EXPECT(display_width() == automatic);
    setenv("HAX_DISPLAY_WIDTH", "999999999999999999999999999", 1);
    EXPECT(display_width() == automatic);
    unsetenv("HAX_DISPLAY_WIDTH");
}

static void test_reflow_physical_rows(void)
{
    int fitting[] = {10, 80, 0};
    EXPECT(reflow_physical_rows(fitting, 3, 80) == 3);

    int wrapping[] = {81, 160, 161};
    EXPECT(reflow_physical_rows(wrapping, 3, 80) == 7);

    int narrow[] = {3};
    EXPECT(reflow_physical_rows(narrow, 1, 1) == 3);

    EXPECT(reflow_physical_rows(NULL, 0, 80) == 0);
}

int main(void)
{
    test_auto_display_width();
    test_display_width_auto();
    test_display_width_env_override();
    test_reflow_physical_rows();

    T_REPORT();
}
