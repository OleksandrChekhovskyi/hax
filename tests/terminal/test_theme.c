/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "config.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"

/* Every role must have non-NULL open/close under every preset (""
 * allowed), so call sites can emit unconditionally. */
static void check_roles_defined(void)
{
    for (int r = 0; r < THEME_ROLE_COUNT; r++) {
        EXPECT(theme_open((enum theme_role)r) != NULL);
        EXPECT(theme_close((enum theme_role)r) != NULL);
    }
}

static void test_default_is_ansi(void)
{
    /* Before any theme_set/theme_init: the pre-theme palette, byte for
     * byte — render tests and library-style use rely on this. */
    EXPECT_STR_EQ(theme_name(), "ansi");
    EXPECT_STR_EQ(theme_open(THEME_ACCENT), ANSI_BRIGHT_MAGENTA);
    EXPECT_STR_EQ(theme_close(THEME_ACCENT), ANSI_FG_DEFAULT);
    EXPECT_STR_EQ(theme_open(THEME_CHROME), ANSI_CYAN);
    /* In the ANSI palette the quiet variant is the same cyan with the
     * role carrying SGR dim itself, which terminals honor on basic
     * colors; the closer must undo both. */
    EXPECT_STR_EQ(theme_open(THEME_CHROME_DIM), ANSI_DIM ANSI_CYAN);
    EXPECT_STR_EQ(theme_close(THEME_CHROME_DIM), ANSI_FG_DEFAULT ANSI_BOLD_OFF);
    EXPECT_STR_EQ(theme_open(THEME_CODE_INLINE), ANSI_CYAN);
    EXPECT_STR_EQ(theme_open(THEME_CODE_BLOCK), ANSI_DIM);
    EXPECT_STR_EQ(theme_close(THEME_CODE_BLOCK), ANSI_BOLD_OFF);
    EXPECT_STR_EQ(theme_open(THEME_HEADING), ANSI_BOLD);
    EXPECT_STR_EQ(theme_close(THEME_HEADING), ANSI_BOLD_OFF);
    EXPECT_STR_EQ(theme_open(THEME_ADD), ANSI_GREEN);
    EXPECT_STR_EQ(theme_open(THEME_REMOVE), ANSI_RED);
    EXPECT_STR_EQ(theme_open(THEME_ERROR), ANSI_RED);
    EXPECT_STR_EQ(theme_open(THEME_WARN), ANSI_YELLOW);
    check_roles_defined();
}

static void test_presets(void)
{
    EXPECT(theme_set("dark") == 0);
    EXPECT_STR_EQ(theme_name(), "dark");
    EXPECT(strstr(theme_open(THEME_ACCENT), "38;5;") != NULL);
    /* Faint over indexed colors is unreliable (some terminals apply it,
     * some ignore it), so in rich presets the quiet chrome must be a
     * genuinely different color with no dim attribute mixed in. */
    EXPECT(strstr(theme_open(THEME_CHROME_DIM), "38;5;") != NULL);
    EXPECT(strcmp(theme_open(THEME_CHROME_DIM), theme_open(THEME_CHROME)) != 0);
    EXPECT(strstr(theme_open(THEME_CHROME_DIM), ANSI_DIM) == NULL);
    /* Heading carries bold plus a color; its closer must undo both. */
    EXPECT(strstr(theme_open(THEME_HEADING), "\x1b[1m") != NULL);
    EXPECT(strstr(theme_close(THEME_HEADING), "\x1b[22m") != NULL);
    EXPECT(strstr(theme_close(THEME_HEADING), "\x1b[39m") != NULL);
    /* Block code is a real color here, closed by fg-default not SGR 22. */
    EXPECT(strstr(theme_open(THEME_CODE_BLOCK), "38;5;") != NULL);
    EXPECT_STR_EQ(theme_close(THEME_CODE_BLOCK), ANSI_FG_DEFAULT);
    check_roles_defined();

    EXPECT(theme_set("light") == 0);
    EXPECT_STR_EQ(theme_name(), "light");
    EXPECT(strstr(theme_open(THEME_ACCENT), "38;5;") != NULL);
    EXPECT(strcmp(theme_open(THEME_CHROME_DIM), theme_open(THEME_CHROME)) != 0);
    check_roles_defined();

    EXPECT(theme_set("off") == 0);
    for (int r = 0; r < THEME_ROLE_COUNT; r++) {
        if (r == THEME_HEADING || r == THEME_CODE_BLOCK || r == THEME_CHROME_DIM)
            continue;
        EXPECT_STR_EQ(theme_open((enum theme_role)r), "");
        EXPECT_STR_EQ(theme_close((enum theme_role)r), "");
    }
    /* "off" drops colors but keeps attribute styling: headings stay bold,
     * fence bodies and quiet chrome stay dim, per the documented NO_COLOR
     * semantics. */
    EXPECT_STR_EQ(theme_open(THEME_HEADING), ANSI_BOLD);
    EXPECT_STR_EQ(theme_close(THEME_HEADING), ANSI_BOLD_OFF);
    EXPECT_STR_EQ(theme_open(THEME_CODE_BLOCK), ANSI_DIM);
    EXPECT_STR_EQ(theme_close(THEME_CODE_BLOCK), ANSI_BOLD_OFF);
    EXPECT_STR_EQ(theme_open(THEME_CHROME_DIM), ANSI_DIM);
    EXPECT_STR_EQ(theme_close(THEME_CHROME_DIM), ANSI_BOLD_OFF);

    /* Unknown names fail without disturbing the active preset. */
    EXPECT(theme_set("bogus") == -1);
    EXPECT(theme_set(NULL) == -1);
    EXPECT_STR_EQ(theme_name(), "off");

    EXPECT(theme_set("ansi") == 0);
    EXPECT_STR_EQ(theme_open(THEME_ACCENT), ANSI_BRIGHT_MAGENTA);
}

static void test_autodetect(void)
{
    /* NO_COLOR (non-empty) beats everything. */
    setenv("NO_COLOR", "1", 1);
    setenv("TERM", "xterm-256color", 1);
    EXPECT(theme_set("auto") == 0);
    EXPECT_STR_EQ(theme_name(), "off");
    /* Empty NO_COLOR does not count (no-color.org). */
    setenv("NO_COLOR", "", 1);
    EXPECT(theme_set("auto") == 0);
    EXPECT(strcmp(theme_name(), "off") != 0);
    unsetenv("NO_COLOR");

    /* A dumb terminal can't be assumed to parse SGR at all. */
    setenv("TERM", "dumb", 1);
    EXPECT(theme_set("auto") == 0);
    EXPECT_STR_EQ(theme_name(), "off");

    /* No 256-color capability: the terminal-scheme ANSI palette. */
    setenv("TERM", "vt100", 1);
    unsetenv("COLORTERM");
    EXPECT(theme_set("auto") == 0);
    EXPECT_STR_EQ(theme_name(), "ansi");
    /* ...unless COLORTERM claims better. */
    setenv("COLORTERM", "truecolor", 1);
    EXPECT(theme_set("auto") == 0);
    EXPECT_STR_EQ(theme_name(), "dark");
    unsetenv("COLORTERM");

    /* 256-color TERM: dark unless COLORFGBG names a light background. */
    setenv("TERM", "xterm-256color", 1);
    unsetenv("COLORFGBG");
    EXPECT(theme_set("auto") == 0);
    EXPECT_STR_EQ(theme_name(), "dark");
    setenv("COLORFGBG", "0;15", 1);
    EXPECT(theme_set("auto") == 0);
    EXPECT_STR_EQ(theme_name(), "light");
    setenv("COLORFGBG", "15;0", 1);
    EXPECT(theme_set("auto") == 0);
    EXPECT_STR_EQ(theme_name(), "dark");
    setenv("COLORFGBG", "12;default;7", 1);
    EXPECT(theme_set("auto") == 0);
    EXPECT_STR_EQ(theme_name(), "light");
    unsetenv("COLORFGBG");
}

static void test_init_from_config(void)
{
    setenv("TERM", "xterm-256color", 1);
    unsetenv("NO_COLOR");

    config_set_override("theme", "light");
    theme_init();
    EXPECT_STR_EQ(theme_name(), "light");

    /* Unknown configured value warns and falls back to auto (dark here). */
    config_set_override("theme", "solarized");
    theme_init();
    EXPECT_STR_EQ(theme_name(), "dark");

    /* Unset resolves to the registry default ("auto"). */
    config_set_override("theme", NULL);
    theme_init();
    EXPECT_STR_EQ(theme_name(), "dark");
}

int main(void)
{
    test_default_is_ansi();
    test_presets();
    test_autodetect();
    test_init_from_config();
    T_REPORT();
}
