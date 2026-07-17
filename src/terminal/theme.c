/* SPDX-License-Identifier: MIT */
#include "terminal/theme.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "terminal/ansi.h"
#include "util.h"

/* A preset is two string tables indexed by role. Missing entries read as
 * "" via the accessors, so the "off" preset needs no rows at all.
 *
 * The "ansi" preset reproduces the pre-theme palette byte-for-byte from
 * the classic 16-color SGRs — the terminal's own scheme decides the
 * actual colors, so it is automatically dark/light safe and stays the
 * right choice for deliberately themed terminals (and for terminals
 * without 256-color support).
 *
 * "dark" and "light" use fixed xterm-256 palette entries picked for
 * mid-luminance readability on the respective background. Truecolor is
 * deliberately not required: 256-color works everywhere modern,
 * including Terminal.app, which still lacks 24-bit SGR. */

struct theme {
    const char *name;
    const char *open[THEME_ROLE_COUNT];
    const char *close[THEME_ROLE_COUNT];
};

#define FG256(n) "\x1b[38;5;" #n "m"

/* clang-format off */
static const struct theme THEMES[] = {
    {
        .name = "ansi",
        .open = {
            [THEME_ACCENT]      = ANSI_BRIGHT_MAGENTA,
            [THEME_CHROME]      = ANSI_CYAN,
            [THEME_CHROME_DIM]  = ANSI_DIM ANSI_CYAN, /* SGR dim quiets basic colors reliably */
            [THEME_CODE_INLINE] = ANSI_CYAN,
            [THEME_CODE_BLOCK]  = ANSI_DIM,
            [THEME_HEADING]     = ANSI_BOLD,
            [THEME_ADD]         = ANSI_GREEN,
            [THEME_REMOVE]      = ANSI_RED,
            [THEME_OK]          = ANSI_GREEN,
            [THEME_ERROR]       = ANSI_RED,
            [THEME_WARN]        = ANSI_YELLOW,
        },
        .close = {
            [THEME_ACCENT]      = ANSI_FG_DEFAULT,
            [THEME_CHROME]      = ANSI_FG_DEFAULT,
            [THEME_CHROME_DIM]  = ANSI_FG_DEFAULT ANSI_BOLD_OFF, /* SGR 22 closes dim */
            [THEME_CODE_INLINE] = ANSI_FG_DEFAULT,
            [THEME_CODE_BLOCK]  = ANSI_BOLD_OFF, /* SGR 22 closes dim */
            [THEME_HEADING]     = ANSI_BOLD_OFF,
            [THEME_ADD]         = ANSI_FG_DEFAULT,
            [THEME_REMOVE]      = ANSI_FG_DEFAULT,
            [THEME_OK]          = ANSI_FG_DEFAULT,
            [THEME_ERROR]       = ANSI_FG_DEFAULT,
            [THEME_WARN]        = ANSI_FG_DEFAULT,
        },
    },
    {
        /* Warm accent against cool chrome: machine output stays teal
         * and gray, so the one warm color on screen marks the user —
         * prompts and typed input stand out in scrollback by hue, not
         * brightness. */
        .name = "dark",
        .open = {
            [THEME_ACCENT]      = FG256(173),
            [THEME_CHROME]      = FG256(37),
            [THEME_CHROME_DIM]  = FG256(23),
            [THEME_CODE_INLINE] = FG256(38),
            [THEME_CODE_BLOCK]  = FG256(31),
            [THEME_HEADING]     = ANSI_BOLD FG256(38),
            [THEME_ADD]         = FG256(28),
            [THEME_REMOVE]      = FG256(124),
            [THEME_OK]          = FG256(28),
            [THEME_ERROR]       = FG256(160),
            [THEME_WARN]        = FG256(178),
        },
        .close = {
            [THEME_ACCENT]      = ANSI_FG_DEFAULT,
            [THEME_CHROME]      = ANSI_FG_DEFAULT,
            [THEME_CHROME_DIM]  = ANSI_FG_DEFAULT,
            [THEME_CODE_INLINE] = ANSI_FG_DEFAULT,
            [THEME_CODE_BLOCK]  = ANSI_FG_DEFAULT,
            [THEME_HEADING]     = ANSI_BOLD_OFF ANSI_FG_DEFAULT,
            [THEME_ADD]         = ANSI_FG_DEFAULT,
            [THEME_REMOVE]      = ANSI_FG_DEFAULT,
            [THEME_OK]          = ANSI_FG_DEFAULT,
            [THEME_ERROR]       = ANSI_FG_DEFAULT,
            [THEME_WARN]        = ANSI_FG_DEFAULT,
        },
    },
    {
        .name = "light",
        .open = {
            [THEME_ACCENT]      = FG256(130),
            [THEME_CHROME]      = FG256(30),
            [THEME_CHROME_DIM]  = FG256(37),
            [THEME_CODE_INLINE] = FG256(31),
            [THEME_CODE_BLOCK]  = FG256(38),
            [THEME_HEADING]     = ANSI_BOLD FG256(31),
            [THEME_ADD]         = FG256(28),
            [THEME_REMOVE]      = FG256(124),
            [THEME_OK]          = FG256(28),
            [THEME_ERROR]       = FG256(160),
            [THEME_WARN]        = FG256(136),
        },
        .close = {
            [THEME_ACCENT]      = ANSI_FG_DEFAULT,
            [THEME_CHROME]      = ANSI_FG_DEFAULT,
            [THEME_CHROME_DIM]  = ANSI_FG_DEFAULT,
            [THEME_CODE_INLINE] = ANSI_FG_DEFAULT,
            [THEME_CODE_BLOCK]  = ANSI_FG_DEFAULT,
            [THEME_HEADING]     = ANSI_BOLD_OFF ANSI_FG_DEFAULT,
            [THEME_ADD]         = ANSI_FG_DEFAULT,
            [THEME_REMOVE]      = ANSI_FG_DEFAULT,
            [THEME_OK]          = ANSI_FG_DEFAULT,
            [THEME_ERROR]       = ANSI_FG_DEFAULT,
            [THEME_WARN]        = ANSI_FG_DEFAULT,
        },
    },
    {
        /* "off" means "no colors", matching NO_COLOR semantics: color
         * roles read as "" via the accessors, while attributes (bold,
         * dim, italic) survive — both the ones call sites emit directly
         * and the ones carried by roles (quiet chrome, heading, code
         * block), which keep their attribute-only styling here. */
        .name = "off",
        .open = {
            [THEME_CHROME_DIM] = ANSI_DIM,
            [THEME_CODE_BLOCK] = ANSI_DIM,
            [THEME_HEADING]    = ANSI_BOLD,
        },
        .close = {
            [THEME_CHROME_DIM] = ANSI_BOLD_OFF, /* SGR 22 closes dim */
            [THEME_CODE_BLOCK] = ANSI_BOLD_OFF,
            [THEME_HEADING]    = ANSI_BOLD_OFF,
        },
    },
};
/* clang-format on */

static const struct theme *active = &THEMES[0]; /* "ansi" until theme_init */

const char *theme_open(enum theme_role role)
{
    const char *s = active->open[role];
    return s ? s : "";
}

const char *theme_close(enum theme_role role)
{
    const char *s = active->close[role];
    return s ? s : "";
}

const char *theme_name(void)
{
    return active->name;
}

/* Pick a preset from the terminal environment. NO_COLOR (non-empty, per
 * no-color.org) disables colors outright. A dumb/absent TERM can't be
 * assumed to parse 256-color SGRs, and without "256color" in TERM (or
 * any COLORTERM claim) the fixed palettes would render as approximations
 * at best — fall back to the terminal-scheme-defined ANSI palette.
 * Dark vs light: COLORFGBG ("<fg>;...;<bg>", set by rxvt/konsole and
 * some emulators) is the only widespread hint that needs no terminal
 * round-trip; background 7 or 15 means light. Absent that, assume dark —
 * the common default — and the fixed palettes are mid-luminance enough
 * to stay legible when the guess is wrong. */
static const char *autodetect(void)
{
    const char *nc = getenv("NO_COLOR");
    if (nc && *nc)
        return "off";
    const char *term = getenv("TERM");
    if (!term || !*term || strcmp(term, "dumb") == 0)
        return "off";
    const char *colorterm = getenv("COLORTERM");
    if (!strstr(term, "256color") && !(colorterm && *colorterm))
        return "ansi";
    const char *fgbg = getenv("COLORFGBG");
    if (fgbg) {
        const char *semi = strrchr(fgbg, ';');
        if (semi && (strcmp(semi + 1, "7") == 0 || strcmp(semi + 1, "15") == 0))
            return "light";
    }
    return "dark";
}

int theme_set(const char *name)
{
    if (!name)
        return -1;
    if (strcmp(name, "auto") == 0)
        name = autodetect();
    for (size_t i = 0; i < sizeof(THEMES) / sizeof(THEMES[0]); i++) {
        if (strcmp(THEMES[i].name, name) == 0) {
            active = &THEMES[i];
            return 0;
        }
    }
    return -1;
}

void theme_init(void)
{
    const char *want = config_str_nonempty("theme");
    if (!want)
        want = "auto";
    if (theme_set(want) != 0) {
        /* Activate the fallback before warning so the warning itself is
         * already themed (NO_COLOR must mute it, for one). */
        theme_set("auto");
        hax_warn("unknown theme '%s' (expected auto, dark, light, ansi, or off)", want);
    }
}
