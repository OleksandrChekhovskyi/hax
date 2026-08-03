/* SPDX-License-Identifier: MIT */
#include "terminal/theme.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "util.h"
#include "terminal/ansi.h"

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

/* Which row of a tint a preset takes — the fixed palettes pick by
 * background; ansi and off take none (see theme.h). */
enum tint_row { TINT_ROW_NONE = 0, TINT_ROW_DARK, TINT_ROW_LIGHT };

struct theme {
    const char *name;
    enum tint_row row;
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
            [THEME_STANCE]      = ANSI_CYAN,
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
            [THEME_STANCE]      = ANSI_FG_DEFAULT,
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
        .row = TINT_ROW_DARK,
        .open = {
            [THEME_ACCENT]      = FG256(173),
            [THEME_CHROME]      = FG256(37),
            [THEME_CHROME_DIM]  = FG256(23),
            [THEME_STANCE]      = FG256(38),
            [THEME_CODE_INLINE] = FG256(38),
            [THEME_CODE_BLOCK]  = FG256(31),
            [THEME_HEADING]     = ANSI_BOLD FG256(38),
            [THEME_ADD]         = FG256(34),
            [THEME_REMOVE]      = FG256(160),
            [THEME_OK]          = FG256(28),
            [THEME_ERROR]       = FG256(160),
            [THEME_WARN]        = FG256(178),
        },
        .close = {
            [THEME_ACCENT]      = ANSI_FG_DEFAULT,
            [THEME_CHROME]      = ANSI_FG_DEFAULT,
            [THEME_CHROME_DIM]  = ANSI_FG_DEFAULT,
            [THEME_STANCE]      = ANSI_FG_DEFAULT,
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
        .row = TINT_ROW_LIGHT,
        .open = {
            [THEME_ACCENT]      = FG256(130),
            [THEME_CHROME]      = FG256(30),
            [THEME_CHROME_DIM]  = FG256(37),
            [THEME_STANCE]      = FG256(31),
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
            [THEME_STANCE]      = ANSI_FG_DEFAULT,
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

/* A tint row recolors the model's voice: the stance token and inline code
 * share one hue, fence bodies take the quieter sibling (darker on a dark
 * background, lighter on a light one, like the base palettes), and the
 * heading is that same hue plus bold.
 *
 * The picks sit in the 6x6x6 cube's low-chroma interior — no component at
 * 0 or 255 — while the status roles stay on its saturated corners: ADD is
 * (0,175,0), REMOVE (215,0,0), WARN (215,175,0). That chroma gap, not
 * hue, is what keeps "rose" from reading as removed and "sage" as added.
 * It is also why no tint enters the wedge around ACCENT's terracotta
 * (215,135,95), which marks the user and must stay unmistakable.
 *
 * There is a floor to that gap as well as a ceiling: a tint also has to
 * stay clear of the *gray* axis, or it reads as washed-out body text
 * rather than as an identity. Keep the chroma spread (max channel minus
 * min) at 80 or more when retuning — sage first shipped at 40 and was the
 * one nobody could see.
 *
 * "teal" carries no rows at all: it *is* the base palette, so the table
 * never restates it and the two cannot drift apart. */

enum tint_slot { TINT_INLINE, TINT_BLOCK, TINT_HEADING, TINT_SLOT_COUNT };

struct tint {
    const char *name;
    const char *dark[TINT_SLOT_COUNT];
    const char *light[TINT_SLOT_COUNT];
};

/* clang-format off */
static const struct tint TINTS[] = {
    {.name = "teal"}, /* the presets' own palette — no overlay */
    {
        .name  = "violet",
        .dark  = {FG256(140), FG256(97),  ANSI_BOLD FG256(140)},
        .light = {FG256(97),  FG256(140), ANSI_BOLD FG256(97)},
    },
    {
        .name  = "rose",
        .dark  = {FG256(168), FG256(132), ANSI_BOLD FG256(168)},
        .light = {FG256(132), FG256(168), ANSI_BOLD FG256(132)},
    },
    {
        .name  = "sage",
        .dark  = {FG256(114), FG256(71),  ANSI_BOLD FG256(114)},
        .light = {FG256(71),  FG256(114), ANSI_BOLD FG256(71)},
    },
};
/* clang-format on */

static const struct theme *active = &THEMES[0]; /* "ansi" until theme_init */
static const struct tint *tint = &TINTS[0];     /* "teal" until theme_init */
static const char *const *tint_rows;            /* active overlay, or NULL */

static const struct tint *tint_find(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof(TINTS) / sizeof(TINTS[0]); i++) {
        if (strcasecmp(TINTS[i].name, name) == 0)
            return &TINTS[i];
    }
    return NULL;
}

/* The overlay `t` supplies under the active preset, or NULL when the preset
 * takes no tint (ansi, off) — the one place the two axes are paired, so
 * activating a tint and previewing one can't diverge. */
static const char *const *tint_overlay(const struct tint *t)
{
    if (active->row == TINT_ROW_DARK)
        return t->dark;
    if (active->row == TINT_ROW_LIGHT)
        return t->light;
    return NULL;
}

/* Re-pair the two axes: either setter can run first, and the preset
 * decides whether the tint applies at all. */
static void tint_resolve(void)
{
    tint_rows = tint_overlay(tint);
}

static int tint_slot(enum theme_role role)
{
    switch (role) {
    case THEME_STANCE:
    case THEME_CODE_INLINE:
        return TINT_INLINE;
    case THEME_CODE_BLOCK:
        return TINT_BLOCK;
    case THEME_HEADING:
        return TINT_HEADING;
    default:
        return -1;
    }
}

/* Resolve `role` under `rows` (an overlay, or NULL for none): the overlay's
 * entry where it has one, else the active preset's own. "teal" carries no rows
 * at all and a tinted preset leaves the untinted roles empty, so the fallback
 * is load-bearing in both directions. */
static const char *role_open(const char *const *rows, enum theme_role role)
{
    if (rows) {
        int slot = tint_slot(role);
        if (slot >= 0 && rows[slot])
            return rows[slot];
    }
    const char *s = active->open[role];
    return s ? s : "";
}

const char *theme_open(enum theme_role role)
{
    return role_open(tint_rows, role);
}

/* No tint counterpart: under the fixed palettes every tinted role already
 * closes with fg-default (the heading also with SGR 22), and the presets
 * that close differently — ansi, off — take no tint. So a tint changes
 * how a role opens, never how it closes. */
const char *theme_close(enum theme_role role)
{
    const char *s = active->close[role];
    return s ? s : "";
}

const char *theme_name(void)
{
    return active->name;
}

const char *theme_tint_name(void)
{
    return tint->name;
}

const char *theme_tint_open(const char *name)
{
    const struct tint *t = tint_find(name);
    if (!t)
        return NULL;
    const char *const *rows = tint_overlay(t);
    /* No overlay under this preset means no tint applies at all (ansi, off) —
     * distinct from an overlay that leaves a role to the preset, which
     * role_open resolves. Falling through would preview every tint as the
     * preset's own color. */
    if (!rows)
        return NULL;
    /* Deliberately not theme_open: that resolves against the *active* tint,
     * which is exactly what a preview must not do. */
    return role_open(rows, THEME_STANCE);
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
    /* Case-insensitive to match the config enum validator (and the other
     * enum consumers, notify/sort_models), so HAX_THEME=LIGHT resolves rather
     * than warning and falling back to auto. */
    if (strcasecmp(name, "auto") == 0)
        name = autodetect();
    for (size_t i = 0; i < sizeof(THEMES) / sizeof(THEMES[0]); i++) {
        if (strcasecmp(THEMES[i].name, name) == 0) {
            active = &THEMES[i];
            tint_resolve();
            return 0;
        }
    }
    return -1;
}

int theme_tint_set(const char *name)
{
    const struct tint *t = tint_find(name);
    if (!t)
        return -1;
    tint = t;
    tint_resolve();
    return 0;
}

/* Whether `value` is a new mistake worth a line. Resolution runs often —
 * twice at startup (before and after a preset applies), then on every preset
 * switch and /config edit — and an unresolvable name is one mistake in one
 * config file, so each distinct one is reported once. Deduping by value
 * rather than by call count is what keeps a name that only becomes effective
 * later reportable: a stance can supply a valid tint at startup and mask a
 * broken HAX_TINT until the stance ends. */
static int first_report(char **seen, const char *value)
{
    if (*seen && strcmp(*seen, value) == 0)
        return 0;
    free(*seen);
    *seen = xstrdup(value);
    return 1;
}

void theme_init(void)
{
    static char *warned_theme, *warned_tint;
    const char *want = config_str("theme");
    if (!want)
        want = "auto";
    if (theme_set(want) != 0) {
        /* Activate the fallback before warning so the warning itself is
         * already themed (NO_COLOR must mute it, for one). */
        theme_set("auto");
        if (first_report(&warned_theme, want))
            hax_warn("unknown theme '%s' (expected auto, dark, light, ansi, or off)", want);
    }
    /* Tint precedence mirrors the selection keys: an explicit runtime choice
     * (a /config tint, which lands in the run tier) outranks the active
     * stance, which outranks HAX_TINT and the config file. config_preset_apply
     * clears the run tier, so a run-tier value here is one chosen *since* the
     * stance was applied — newest wins, as everywhere else in that tier.
     *
     * Empty is not a tint the way it is a "send no system prompt": nothing
     * names one, so the base palette applies. */
    const char *want_tint = NULL;
    if (strcmp(config_source("tint"), "run") == 0)
        want_tint = config_str("tint");
    if (!want_tint || !*want_tint) {
        const char *stance = config_str("preset");
        want_tint = (stance && *stance) ? config_preset_tint(stance) : NULL;
    }
    if (!want_tint || !*want_tint)
        want_tint = config_str("tint");
    if (!want_tint || !*want_tint)
        want_tint = "teal";
    if (theme_tint_set(want_tint) != 0) {
        theme_tint_set("teal");
        if (first_report(&warned_tint, want_tint))
            hax_warn("unknown tint '%s' (expected teal, violet, rose, or sage)", want_tint);
    }
}
