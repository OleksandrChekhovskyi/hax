/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "diag.h"
#include "harness.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"

static const enum theme_role TINTED_ROLES[] = {THEME_STANCE, THEME_CODE_INLINE, THEME_CODE_BLOCK,
                                               THEME_HEADING, THEME_LINK};
static const enum theme_role FIXED_ROLES[] = {THEME_ACCENT, THEME_CHROME, THEME_CHROME_DIM,
                                              THEME_ADD,    THEME_REMOVE, THEME_OK,
                                              THEME_ERROR,  THEME_WARN};
static const char *const TINT_NAMES[] = {"teal", "violet", "rose", "sage"};

static void expect_all_roles_defined(void)
{
    for (int role = 0; role < THEME_ROLE_COUNT; role++) {
        EXPECT(theme_open((enum theme_role)role) != NULL);
        EXPECT(theme_close((enum theme_role)role) != NULL);
    }
}

static void test_default_is_ansi(void)
{
    EXPECT_STR_EQ(theme_name(), "ansi");
    EXPECT_STR_EQ(theme_open(THEME_ACCENT), ANSI_BRIGHT_MAGENTA);
    EXPECT_STR_EQ(theme_close(THEME_ACCENT), ANSI_FG_DEFAULT);
    EXPECT_STR_EQ(theme_open(THEME_CHROME), ANSI_CYAN);
    EXPECT_STR_EQ(theme_open(THEME_CHROME_DIM), ANSI_DIM ANSI_CYAN);
    EXPECT_STR_EQ(theme_close(THEME_CHROME_DIM), ANSI_FG_DEFAULT ANSI_BOLD_OFF);
    EXPECT_STR_EQ(theme_open(THEME_STANCE), ANSI_CYAN);
    EXPECT_STR_EQ(theme_open(THEME_CODE_INLINE), ANSI_CYAN);
    EXPECT_STR_EQ(theme_open(THEME_CODE_BLOCK), ANSI_DIM);
    EXPECT_STR_EQ(theme_close(THEME_CODE_BLOCK), ANSI_BOLD_OFF);
    EXPECT_STR_EQ(theme_open(THEME_HEADING), ANSI_BOLD);
    EXPECT_STR_EQ(theme_close(THEME_HEADING), ANSI_BOLD_OFF);
    EXPECT_STR_EQ(theme_open(THEME_LINK), ANSI_UNDERLINE);
    EXPECT_STR_EQ(theme_close(THEME_LINK), ANSI_UNDERLINE_OFF);
    EXPECT_STR_EQ(theme_open(THEME_ADD), ANSI_GREEN);
    EXPECT_STR_EQ(theme_open(THEME_REMOVE), ANSI_RED);
    EXPECT_STR_EQ(theme_open(THEME_ERROR), ANSI_RED);
    EXPECT_STR_EQ(theme_open(THEME_WARN), ANSI_YELLOW);
    expect_all_roles_defined();
}

static void test_indexed_themes(void)
{
    EXPECT(theme_set("dark") == 0);
    EXPECT_STR_EQ(theme_name(), "dark");
    EXPECT(strstr(theme_open(THEME_ACCENT), "38;5;") != NULL);
    EXPECT(strstr(theme_open(THEME_CHROME_DIM), "38;5;") != NULL);
    EXPECT(strcmp(theme_open(THEME_CHROME_DIM), theme_open(THEME_CHROME)) != 0);
    EXPECT(strstr(theme_open(THEME_CHROME_DIM), ANSI_DIM) == NULL);
    EXPECT(strstr(theme_open(THEME_HEADING), ANSI_BOLD) != NULL);
    EXPECT(strstr(theme_close(THEME_HEADING), ANSI_BOLD_OFF) != NULL);
    EXPECT(strstr(theme_close(THEME_HEADING), ANSI_FG_DEFAULT) != NULL);
    EXPECT(strstr(theme_open(THEME_CODE_BLOCK), "38;5;") != NULL);
    EXPECT_STR_EQ(theme_close(THEME_CODE_BLOCK), ANSI_FG_DEFAULT);
    /* Link open and close are single sequences so table replay can match them whole. */
    EXPECT(strstr(theme_open(THEME_LINK), "4;38;5;") != NULL);
    EXPECT(strstr(theme_open(THEME_LINK), "m\x1b") == NULL);
    EXPECT(strstr(theme_close(THEME_LINK), "24") != NULL);
    EXPECT(strstr(theme_close(THEME_LINK), "m\x1b") == NULL);
    expect_all_roles_defined();

    EXPECT(theme_set("light") == 0);
    EXPECT_STR_EQ(theme_name(), "light");
    EXPECT(strstr(theme_open(THEME_ACCENT), "38;5;") != NULL);
    EXPECT(strcmp(theme_open(THEME_CHROME_DIM), theme_open(THEME_CHROME)) != 0);
    expect_all_roles_defined();
}

static void test_off_theme_preserves_attributes(void)
{
    EXPECT(theme_set("off") == 0);
    for (int role = 0; role < THEME_ROLE_COUNT; role++) {
        if (role == THEME_HEADING || role == THEME_CODE_BLOCK || role == THEME_CHROME_DIM ||
            role == THEME_LINK)
            continue;
        EXPECT_STR_EQ(theme_open((enum theme_role)role), "");
        EXPECT_STR_EQ(theme_close((enum theme_role)role), "");
    }
    EXPECT_STR_EQ(theme_open(THEME_HEADING), ANSI_BOLD);
    EXPECT_STR_EQ(theme_close(THEME_HEADING), ANSI_BOLD_OFF);
    EXPECT_STR_EQ(theme_open(THEME_CODE_BLOCK), ANSI_DIM);
    EXPECT_STR_EQ(theme_close(THEME_CODE_BLOCK), ANSI_BOLD_OFF);
    EXPECT_STR_EQ(theme_open(THEME_CHROME_DIM), ANSI_DIM);
    EXPECT_STR_EQ(theme_close(THEME_CHROME_DIM), ANSI_BOLD_OFF);
    EXPECT_STR_EQ(theme_open(THEME_LINK), ANSI_UNDERLINE);
    EXPECT_STR_EQ(theme_close(THEME_LINK), ANSI_UNDERLINE_OFF);
}

static void test_theme_set_validation(void)
{
    EXPECT(theme_set("LIGHT") == 0);
    EXPECT_STR_EQ(theme_name(), "light");
    EXPECT(theme_set("Off") == 0);
    EXPECT_STR_EQ(theme_name(), "off");

    EXPECT(theme_set("bogus") == -1);
    EXPECT(theme_set(NULL) == -1);
    EXPECT_STR_EQ(theme_name(), "off");

    EXPECT(theme_set("ansi") == 0);
    EXPECT_STR_EQ(theme_open(THEME_ACCENT), ANSI_BRIGHT_MAGENTA);
}

static void test_tint_roles(void)
{
    EXPECT(theme_set("dark") == 0);
    EXPECT(theme_tint_set("teal") == 0);

    const char *base_open[THEME_ROLE_COUNT];
    const char *base_close[THEME_ROLE_COUNT];
    for (int role = 0; role < THEME_ROLE_COUNT; role++) {
        base_open[role] = theme_open((enum theme_role)role);
        base_close[role] = theme_close((enum theme_role)role);
    }

    EXPECT(theme_tint_set("violet") == 0);
    EXPECT_STR_EQ(theme_tint_name(), "violet");
    for (size_t i = 0; i < sizeof(TINTED_ROLES) / sizeof(TINTED_ROLES[0]); i++)
        EXPECT(strcmp(theme_open(TINTED_ROLES[i]), base_open[TINTED_ROLES[i]]) != 0);
    for (size_t i = 0; i < sizeof(FIXED_ROLES) / sizeof(FIXED_ROLES[0]); i++)
        EXPECT_STR_EQ(theme_open(FIXED_ROLES[i]), base_open[FIXED_ROLES[i]]);
    for (int role = 0; role < THEME_ROLE_COUNT; role++)
        EXPECT_STR_EQ(theme_close((enum theme_role)role), base_close[role]);

    EXPECT_STR_EQ(theme_open(THEME_STANCE), theme_open(THEME_CODE_INLINE));
    EXPECT(strstr(theme_open(THEME_HEADING), ANSI_BOLD) != NULL);
    EXPECT(strstr(theme_open(THEME_HEADING), theme_open(THEME_CODE_INLINE)) != NULL);
    EXPECT(strcmp(theme_open(THEME_CODE_BLOCK), theme_open(THEME_CODE_INLINE)) != 0);
    expect_all_roles_defined();

    EXPECT(theme_tint_set("teal") == 0);
    for (int role = 0; role < THEME_ROLE_COUNT; role++)
        EXPECT_STR_EQ(theme_open((enum theme_role)role), base_open[role]);
}

static void test_tint_palettes(void)
{
    EXPECT(theme_set("dark") == 0);
    const size_t tint_count = sizeof(TINT_NAMES) / sizeof(TINT_NAMES[0]);
    const char *tint_opens[sizeof(TINT_NAMES) / sizeof(TINT_NAMES[0])];
    for (size_t i = 0; i < tint_count; i++) {
        EXPECT(theme_tint_set(TINT_NAMES[i]) == 0);
        tint_opens[i] = theme_open(THEME_CODE_INLINE);
    }
    for (size_t i = 0; i < tint_count; i++)
        for (size_t j = i + 1; j < tint_count; j++)
            EXPECT(strcmp(tint_opens[i], tint_opens[j]) != 0);

    EXPECT(theme_tint_set("rose") == 0);
    EXPECT(theme_set("light") == 0);
    const char *light_rose = theme_open(THEME_CODE_INLINE);
    EXPECT(strstr(light_rose, "38;5;") != NULL);
    EXPECT(theme_set("dark") == 0);
    EXPECT(strcmp(theme_open(THEME_CODE_INLINE), light_rose) != 0);
}

static void test_themes_without_tints(void)
{
    EXPECT(theme_set("dark") == 0);
    EXPECT(theme_tint_set("teal") == 0);
    const char *base_inline = theme_open(THEME_CODE_INLINE);
    EXPECT(theme_tint_set("rose") == 0);

    EXPECT(theme_set("ansi") == 0);
    EXPECT_STR_EQ(theme_open(THEME_STANCE), ANSI_CYAN);
    EXPECT_STR_EQ(theme_open(THEME_CODE_INLINE), ANSI_CYAN);
    EXPECT(theme_set("off") == 0);
    EXPECT_STR_EQ(theme_open(THEME_STANCE), "");
    EXPECT_STR_EQ(theme_open(THEME_CODE_INLINE), "");
    EXPECT_STR_EQ(theme_tint_name(), "rose");
    EXPECT(theme_set("dark") == 0);
    EXPECT(strcmp(theme_open(THEME_CODE_INLINE), base_inline) != 0);
}

static void test_tint_set_validation(void)
{
    EXPECT(theme_tint_set("SAGE") == 0);
    EXPECT_STR_EQ(theme_tint_name(), "sage");
    EXPECT(theme_tint_set("chartreuse") == -1);
    EXPECT(theme_tint_set(NULL) == -1);
    EXPECT_STR_EQ(theme_tint_name(), "sage");
    EXPECT(theme_tint_set("teal") == 0);
}

static void test_tint_preview(void)
{
    EXPECT(theme_set("dark") == 0);
    EXPECT(theme_tint_set("rose") == 0);
    const size_t tint_count = sizeof(TINT_NAMES) / sizeof(TINT_NAMES[0]);
    const char *previews[sizeof(TINT_NAMES) / sizeof(TINT_NAMES[0])];
    for (size_t i = 0; i < tint_count; i++) {
        previews[i] = theme_tint_open(TINT_NAMES[i]);
        EXPECT(previews[i] != NULL && strstr(previews[i], "38;5;") != NULL);
        if (strcmp(TINT_NAMES[i], theme_tint_name()) == 0)
            EXPECT_STR_EQ(previews[i], theme_open(THEME_STANCE));
    }
    for (size_t i = 0; i < tint_count; i++)
        for (size_t j = i + 1; j < tint_count; j++)
            EXPECT(strcmp(previews[i], previews[j]) != 0);

    const char *teal = theme_tint_open("teal");
    EXPECT(theme_tint_set("teal") == 0);
    EXPECT_STR_EQ(teal, theme_open(THEME_STANCE));

    const char *dark_sage = theme_tint_open("sage");
    EXPECT(theme_set("light") == 0);
    EXPECT(strcmp(theme_tint_open("sage"), dark_sage) != 0);

    EXPECT(theme_set("ansi") == 0);
    EXPECT(theme_tint_open("sage") == NULL);
    EXPECT(theme_set("off") == 0);
    EXPECT(theme_tint_open("sage") == NULL);

    EXPECT(theme_set("dark") == 0);
    EXPECT(theme_tint_open("chartreuse") == NULL);
    EXPECT(theme_tint_open(NULL) == NULL);
}

static void test_no_color_autodetection(void)
{
    setenv("TERM", "xterm-256color", 1);
    setenv("NO_COLOR", "1", 1);
    EXPECT(theme_set("auto") == 0);
    EXPECT_STR_EQ(theme_name(), "off");

    setenv("NO_COLOR", "", 1);
    EXPECT(theme_set("auto") == 0);
    EXPECT(strcmp(theme_name(), "off") != 0);
    unsetenv("NO_COLOR");
}

static void test_terminal_capability_autodetection(void)
{
    unsetenv("NO_COLOR");
    unsetenv("COLORFGBG");
    setenv("TERM", "dumb", 1);
    EXPECT(theme_set("auto") == 0);
    EXPECT_STR_EQ(theme_name(), "off");

    setenv("TERM", "vt100", 1);
    unsetenv("COLORTERM");
    EXPECT(theme_set("auto") == 0);
    EXPECT_STR_EQ(theme_name(), "ansi");

    setenv("COLORTERM", "truecolor", 1);
    EXPECT(theme_set("auto") == 0);
    EXPECT_STR_EQ(theme_name(), "dark");
    unsetenv("COLORTERM");
}

static void test_background_autodetection(void)
{
    unsetenv("NO_COLOR");
    unsetenv("COLORTERM");
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

static void test_config_resolution(void)
{
    setenv("TERM", "xterm-256color", 1);
    unsetenv("NO_COLOR");
    unsetenv("COLORFGBG");

    config_set_override("theme", "light");
    theme_init();
    EXPECT_STR_EQ(theme_name(), "light");

    config_set_override("theme", "solarized");
    theme_init();
    EXPECT_STR_EQ(theme_name(), "dark");

    config_set_override("theme", NULL);
    theme_init();
    EXPECT_STR_EQ(theme_name(), "dark");

    config_set_override("tint", "rose");
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "rose");

    config_set_override("tint", "chartreuse");
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "teal");

    config_set_override("tint", "violet");
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "violet");
    config_set_override("tint", NULL);
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "teal");

    config_set_override("theme", "light");
    config_set_override("tint", "sage");
    theme_init();
    EXPECT_STR_EQ(theme_name(), "light");
    EXPECT_STR_EQ(theme_tint_name(), "sage");
    config_set_override("tint", "chartreuse");
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "teal");
    config_set_override("theme", NULL);
    config_set_override("tint", NULL);
}

static void test_preset_tint_precedence(void)
{
    setenv("TERM", "xterm-256color", 1);
    unsetenv("NO_COLOR");
    unsetenv("HAX_TINT");
    config_set_override("theme", "dark");
    config_set_override("tint", NULL);
    EXPECT(config_load("{\"presets\": {\"review\": {\"provider\": \"mock\", \"tint\": \"rose\"},"
                       " \"plain\": {\"provider\": \"mock\"}}}") == 0);

    config_set_override("preset", NULL);
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "teal");

    config_set_override("preset", "review");
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "rose");

    setenv("HAX_TINT", "sage", 1);
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "rose");
    config_set_override("preset", "plain");
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "sage");
    unsetenv("HAX_TINT");

    /* Presets do not write the tint key, so an explicit runtime tint survives preset exit. */
    config_set_override("preset", "review");
    config_set_override("tint", "violet");
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "violet");
    config_preset_exit(CONFIG_TIER_RUN);
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "violet");

    config_set_override("tint", NULL);
    config_set_override("preset", NULL);
    config_set_override("theme", NULL);
    EXPECT(config_load(NULL) == 0);
}

/* Warning deduplication is process-wide, so this value must be unique within the test binary. */
static void test_masked_invalid_tint_warning(void)
{
    setenv("TERM", "xterm-256color", 1);
    unsetenv("NO_COLOR");
    unsetenv("HAX_TINT");
    config_set_override("theme", "dark");
    config_set_override("tint", NULL);
    EXPECT(config_load("{\"tint\": \"ultramarine\", \"presets\":"
                       " {\"review\": {\"provider\": \"mock\", \"tint\": \"rose\"}}}") == 0);

    config_set_override("preset", "review");
    unsigned long diagnostics_before = hax_diag_sequence();
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "rose");
    EXPECT(hax_diag_sequence() == diagnostics_before);

    config_set_override("preset", NULL);
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "teal");
    EXPECT(hax_diag_sequence() == diagnostics_before + 1);

    theme_init();
    theme_init();
    EXPECT(hax_diag_sequence() == diagnostics_before + 1);

    config_set_override("theme", NULL);
    EXPECT(config_load(NULL) == 0);
}

int main(void)
{
    test_default_is_ansi();
    test_indexed_themes();
    test_off_theme_preserves_attributes();
    test_theme_set_validation();
    test_tint_roles();
    test_tint_palettes();
    test_themes_without_tints();
    test_tint_set_validation();
    test_tint_preview();
    test_no_color_autodetection();
    test_terminal_capability_autodetection();
    test_background_autodetection();
    test_config_resolution();
    test_preset_tint_precedence();
    test_masked_invalid_tint_warning();
    T_REPORT();
}
