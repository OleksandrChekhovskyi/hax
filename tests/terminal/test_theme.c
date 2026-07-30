/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "util.h"
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
    EXPECT_STR_EQ(theme_open(THEME_STANCE), ANSI_CYAN);
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

    /* Names match case-insensitively, like the config enum validator, so a
     * HAX_THEME=LIGHT resolves rather than falling back to auto. */
    EXPECT(theme_set("LIGHT") == 0);
    EXPECT_STR_EQ(theme_name(), "light");
    EXPECT(theme_set("Off") == 0);
    EXPECT_STR_EQ(theme_name(), "off");

    /* Unknown names fail without disturbing the active preset. */
    EXPECT(theme_set("bogus") == -1);
    EXPECT(theme_set(NULL) == -1);
    EXPECT_STR_EQ(theme_name(), "off");

    EXPECT(theme_set("ansi") == 0);
    EXPECT_STR_EQ(theme_open(THEME_ACCENT), ANSI_BRIGHT_MAGENTA);
}

/* The model's voice: everything a tint recolors. */
static const enum theme_role TINTED[] = {THEME_STANCE, THEME_CODE_INLINE, THEME_CODE_BLOCK,
                                         THEME_HEADING};

/* What a tint must leave alone — hax's own chrome, the user's marker, and
 * the status vocabulary, whose meanings don't change with the persona. */
static const enum theme_role FIXED[] = {THEME_ACCENT, THEME_CHROME, THEME_CHROME_DIM, THEME_ADD,
                                        THEME_REMOVE, THEME_OK,     THEME_ERROR,      THEME_WARN};

static void test_tints(void)
{
    EXPECT(theme_set("dark") == 0);
    EXPECT(theme_tint_set("teal") == 0);
    EXPECT_STR_EQ(theme_tint_name(), "teal");

    /* Every sequence is a static literal, so these stay valid as the
     * active tint changes underneath them. */
    const char *base_open[THEME_ROLE_COUNT], *base_close[THEME_ROLE_COUNT];
    for (int r = 0; r < THEME_ROLE_COUNT; r++) {
        base_open[r] = theme_open((enum theme_role)r);
        base_close[r] = theme_close((enum theme_role)r);
    }

    EXPECT(theme_tint_set("violet") == 0);
    EXPECT_STR_EQ(theme_tint_name(), "violet");
    for (size_t i = 0; i < sizeof(TINTED) / sizeof(TINTED[0]); i++)
        EXPECT(strcmp(theme_open(TINTED[i]), base_open[TINTED[i]]) != 0);
    for (size_t i = 0; i < sizeof(FIXED) / sizeof(FIXED[0]); i++)
        EXPECT_STR_EQ(theme_open(FIXED[i]), base_open[FIXED[i]]);
    /* A tint changes how a role opens, never how it closes. */
    for (int r = 0; r < THEME_ROLE_COUNT; r++)
        EXPECT_STR_EQ(theme_close((enum theme_role)r), base_close[r]);
    /* The stance token and inline code share the hue, the heading is that
     * hue plus bold, and fence bodies take the quieter sibling. */
    EXPECT_STR_EQ(theme_open(THEME_STANCE), theme_open(THEME_CODE_INLINE));
    EXPECT(strstr(theme_open(THEME_HEADING), ANSI_BOLD) != NULL);
    EXPECT(strstr(theme_open(THEME_HEADING), theme_open(THEME_CODE_INLINE)) != NULL);
    EXPECT(strcmp(theme_open(THEME_CODE_BLOCK), theme_open(THEME_CODE_INLINE)) != 0);
    check_roles_defined();

    /* "teal" is the presets' own palette rather than a copy of it, so it
     * restores every role byte for byte. */
    EXPECT(theme_tint_set("teal") == 0);
    for (int r = 0; r < THEME_ROLE_COUNT; r++)
        EXPECT_STR_EQ(theme_open((enum theme_role)r), base_open[r]);

    /* No two tints share the model's hue — the whole point is telling two
     * personas apart at a glance. */
    static const char *const NAMES[] = {"teal", "violet", "rose", "sage"};
    const size_t n_names = sizeof(NAMES) / sizeof(NAMES[0]);
    const char *hue[sizeof(NAMES) / sizeof(NAMES[0])];
    for (size_t i = 0; i < n_names; i++) {
        EXPECT(theme_tint_set(NAMES[i]) == 0);
        hue[i] = theme_open(THEME_CODE_INLINE);
    }
    for (size_t i = 0; i < n_names; i++)
        for (size_t j = i + 1; j < n_names; j++)
            EXPECT(strcmp(hue[i], hue[j]) != 0);

    /* Either axis may be set first; the pair is re-resolved on every
     * change, and each background gets its own row. */
    EXPECT(theme_tint_set("rose") == 0);
    EXPECT(theme_set("light") == 0);
    const char *light_rose = theme_open(THEME_CODE_INLINE);
    EXPECT(strstr(light_rose, "38;5;") != NULL);
    EXPECT(theme_set("dark") == 0);
    EXPECT(strcmp(theme_open(THEME_CODE_INLINE), light_rose) != 0);

    /* ansi defers to the terminal's own scheme and off has no colors at
     * all, so neither takes a tint — but the selection survives, ready for
     * when a fixed palette comes back. */
    EXPECT(theme_set("ansi") == 0);
    EXPECT_STR_EQ(theme_open(THEME_STANCE), ANSI_CYAN);
    EXPECT_STR_EQ(theme_open(THEME_CODE_INLINE), ANSI_CYAN);
    EXPECT(theme_set("off") == 0);
    EXPECT_STR_EQ(theme_open(THEME_STANCE), "");
    EXPECT_STR_EQ(theme_open(THEME_CODE_INLINE), "");
    EXPECT_STR_EQ(theme_tint_name(), "rose");
    EXPECT(theme_set("dark") == 0);
    EXPECT(strcmp(theme_open(THEME_CODE_INLINE), base_open[THEME_CODE_INLINE]) != 0);

    /* Case-insensitive like the config enum validator; an unknown name
     * fails without disturbing the active tint. */
    EXPECT(theme_tint_set("SAGE") == 0);
    EXPECT_STR_EQ(theme_tint_name(), "sage");
    EXPECT(theme_tint_set("chartreuse") == -1);
    EXPECT(theme_tint_set(NULL) == -1);
    EXPECT_STR_EQ(theme_tint_name(), "sage");
    EXPECT(theme_tint_set("teal") == 0);
}

static void test_tint_preview(void)
{
    /* Each tint's own stance color under the active preset, read without
     * activating it — so a picker shows four colors while one tint is live. */
    EXPECT(theme_set("dark") == 0);
    EXPECT(theme_tint_set("rose") == 0);
    static const char *const NAMES[] = {"teal", "violet", "rose", "sage"};
    const size_t n = sizeof(NAMES) / sizeof(NAMES[0]);
    const char *seen[sizeof(NAMES) / sizeof(NAMES[0])];
    for (size_t i = 0; i < n; i++) {
        seen[i] = theme_tint_open(NAMES[i]);
        EXPECT(seen[i] != NULL && strstr(seen[i], "38;5;") != NULL);
        /* For the live tint, the preview is what theme_open reports. */
        if (strcmp(NAMES[i], theme_tint_name()) == 0)
            EXPECT_STR_EQ(seen[i], theme_open(THEME_STANCE));
    }
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            EXPECT(strcmp(seen[i], seen[j]) != 0);

    /* "teal" is the preset's own palette, not an overlay: it must preview as
     * the untinted stance color rather than as whatever tint is active. */
    const char *teal = theme_tint_open("teal");
    EXPECT(theme_tint_set("teal") == 0);
    EXPECT_STR_EQ(teal, theme_open(THEME_STANCE));

    /* Each background has its own row. */
    const char *dark_sage = theme_tint_open("sage");
    EXPECT(theme_set("light") == 0);
    EXPECT(strcmp(theme_tint_open("sage"), dark_sage) != 0);

    /* No preview where no tint applies — the caller then paints plain rows
     * instead of coloring all four identically. */
    EXPECT(theme_set("ansi") == 0);
    EXPECT(theme_tint_open("sage") == NULL);
    EXPECT(theme_set("off") == 0);
    EXPECT(theme_tint_open("sage") == NULL);

    EXPECT(theme_set("dark") == 0);
    EXPECT(theme_tint_open("chartreuse") == NULL);
    EXPECT(theme_tint_open(NULL) == NULL);
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

    /* The tint resolves on the same pass. */
    config_set_override("tint", "rose");
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "rose");

    /* Unknown configured value warns and falls back to the base palette. */
    config_set_override("tint", "chartreuse");
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "teal");

    /* Cleared — what config_preset_exit does when a stance ends — resolves
     * to the registry default rather than leaving the outgoing hue up. */
    config_set_override("tint", "violet");
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "violet");
    config_set_override("tint", NULL);
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "teal");

    /* Re-resolution is idempotent: both axes track the config on every call,
     * which is what a preset switch and a /config edit rely on. */
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

/* A preset's tint reaches the display without anything writing the "tint"
 * key: the stance names it, and resolution reads it back off the stance. */
static void test_tint_from_stance(void)
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

    /* The stance outranks env; a stance naming no hue falls through to it
     * rather than forcing the default. */
    setenv("HAX_TINT", "sage", 1);
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "rose");
    config_set_override("preset", "plain");
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "sage");
    unsetenv("HAX_TINT");

    /* An explicit runtime tint (/config tint, which lands in the run tier)
     * outranks the stance — and survives the stance ending, which is the
     * whole reason presets don't write this key. */
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

/* Resolution reruns constantly (startup twice, then every preset switch and
 * /config edit), so an unresolvable name is reported once per distinct value
 * rather than once per call. Silencing the reruns instead would look the same
 * from the resolved value alone — but it loses the diagnostic entirely when a
 * stance masks a broken lower-tier tint until it ends, so the count is what
 * this checks. Uses a bogus name no earlier case has spent, since the
 * report-once memory persists for the process. */
static void test_invalid_tint_reported_once(void)
{
    setenv("TERM", "xterm-256color", 1);
    unsetenv("NO_COLOR");
    unsetenv("HAX_TINT");
    config_set_override("theme", "dark");
    config_set_override("tint", NULL);
    EXPECT(config_load("{\"tint\": \"ultramarine\", \"presets\":"
                       " {\"review\": {\"provider\": \"mock\", \"tint\": \"rose\"}}}") == 0);

    /* Masked by the stance: the broken value is never consulted, so there is
     * nothing to report yet. */
    config_set_override("preset", "review");
    unsigned long quiet = hax_diag_sequence();
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "rose");
    EXPECT(hax_diag_sequence() == quiet);

    /* The stance ends, so the broken value becomes what's in effect — and
     * that is when it has to be reported. */
    config_set_override("preset", NULL);
    theme_init();
    EXPECT_STR_EQ(theme_tint_name(), "teal");
    EXPECT(hax_diag_sequence() == quiet + 1);

    /* Once, not once per re-resolution. */
    theme_init();
    theme_init();
    EXPECT(hax_diag_sequence() == quiet + 1);

    config_set_override("theme", NULL);
    EXPECT(config_load(NULL) == 0);
}

int main(void)
{
    test_default_is_ansi();
    test_presets();
    test_tints();
    test_tint_preview();
    test_autodetect();
    test_init_from_config();
    test_tint_from_stance();
    test_invalid_tint_reported_once();
    T_REPORT();
}
