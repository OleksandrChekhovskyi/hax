/* SPDX-License-Identifier: MIT */
#include "terminal/theme.h"

#include <errno.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "diag.h"
#include "xalloc.h"
#include "terminal/ansi.h"

enum tint_palette {
    TINT_PALETTE_NONE = 0,
    TINT_PALETTE_DARK,
    TINT_PALETTE_LIGHT,
};

struct role_style {
    const char *open;
    const char *close;
};

struct theme_preset {
    const char *name;
    enum tint_palette tint_palette;
    struct role_style roles[THEME_ROLE_COUNT];
};

/* Indexed colors remain compatible with terminals that do not support truecolor SGR. */
#define FG256(n) "\x1b[38;5;" #n "m"
/* Single combined sequences so table-cell replay can recognize a link by one escape. */
#define LINK256(n)  ANSI_CSI "4;38;5;" #n "m"
#define LINK256_OFF ANSI_CSI "24;39m"
#define ROLE_STYLE(open_sequence, close_sequence)                                                  \
    {.open = (open_sequence), .close = (close_sequence)}
#define COLOR_STYLE(open_sequence) ROLE_STYLE(open_sequence, ANSI_FG_DEFAULT)

/* Preserve identity by hue when retuning: warm accent marks the user, cool chrome marks the app,
 * and the stance and Markdown roles share a separate model tint. */
/* clang-format off */
static const struct theme_preset THEME_PRESETS[] = {
    {
        .name = "ansi",
        .roles = {
            [THEME_ACCENT]      = COLOR_STYLE(ANSI_BRIGHT_MAGENTA),
            [THEME_CHROME]      = COLOR_STYLE(ANSI_CYAN),
            [THEME_CHROME_DIM]  = ROLE_STYLE(ANSI_DIM ANSI_CYAN, ANSI_FG_DEFAULT ANSI_BOLD_OFF),
            [THEME_STANCE]      = COLOR_STYLE(ANSI_CYAN),
            [THEME_CODE_INLINE] = COLOR_STYLE(ANSI_CYAN),
            [THEME_CODE_BLOCK]  = ROLE_STYLE(ANSI_DIM, ANSI_BOLD_OFF),
            [THEME_HEADING]     = ROLE_STYLE(ANSI_BOLD, ANSI_BOLD_OFF),
            /* Dim would share SGR 22 with bold; underline alone marks links here. */
            [THEME_LINK]        = ROLE_STYLE(ANSI_UNDERLINE, ANSI_UNDERLINE_OFF),
            [THEME_ADD]         = COLOR_STYLE(ANSI_GREEN),
            [THEME_REMOVE]      = COLOR_STYLE(ANSI_RED),
            [THEME_OK]          = COLOR_STYLE(ANSI_GREEN),
            [THEME_ERROR]       = COLOR_STYLE(ANSI_RED),
            [THEME_WARN]        = COLOR_STYLE(ANSI_YELLOW),
        },
    },
    {
        .name = "dark",
        .tint_palette = TINT_PALETTE_DARK,
        .roles = {
            [THEME_ACCENT]      = COLOR_STYLE(FG256(173)),
            [THEME_CHROME]      = COLOR_STYLE(FG256(37)),
            [THEME_CHROME_DIM]  = COLOR_STYLE(FG256(23)),
            [THEME_STANCE]      = COLOR_STYLE(FG256(38)),
            [THEME_CODE_INLINE] = COLOR_STYLE(FG256(38)),
            [THEME_CODE_BLOCK]  = COLOR_STYLE(FG256(31)),
            [THEME_HEADING]     = ROLE_STYLE(ANSI_BOLD FG256(38), ANSI_BOLD_OFF ANSI_FG_DEFAULT),
            [THEME_LINK]        = ROLE_STYLE(LINK256(31), LINK256_OFF),
            [THEME_ADD]         = COLOR_STYLE(FG256(34)),
            [THEME_REMOVE]      = COLOR_STYLE(FG256(160)),
            [THEME_OK]          = COLOR_STYLE(FG256(28)),
            [THEME_ERROR]       = COLOR_STYLE(FG256(160)),
            [THEME_WARN]        = COLOR_STYLE(FG256(178)),
        },
    },
    {
        .name = "light",
        .tint_palette = TINT_PALETTE_LIGHT,
        .roles = {
            [THEME_ACCENT]      = COLOR_STYLE(FG256(130)),
            [THEME_CHROME]      = COLOR_STYLE(FG256(30)),
            [THEME_CHROME_DIM]  = COLOR_STYLE(FG256(37)),
            [THEME_STANCE]      = COLOR_STYLE(FG256(31)),
            [THEME_CODE_INLINE] = COLOR_STYLE(FG256(31)),
            [THEME_CODE_BLOCK]  = COLOR_STYLE(FG256(38)),
            [THEME_HEADING]     = ROLE_STYLE(ANSI_BOLD FG256(31), ANSI_BOLD_OFF ANSI_FG_DEFAULT),
            [THEME_LINK]        = ROLE_STYLE(LINK256(38), LINK256_OFF),
            [THEME_ADD]         = COLOR_STYLE(FG256(28)),
            [THEME_REMOVE]      = COLOR_STYLE(FG256(124)),
            [THEME_OK]          = COLOR_STYLE(FG256(28)),
            [THEME_ERROR]       = COLOR_STYLE(FG256(160)),
            [THEME_WARN]        = COLOR_STYLE(FG256(136)),
        },
    },
    {
        .name = "off",
        .roles = {
            [THEME_CHROME_DIM] = ROLE_STYLE(ANSI_DIM, ANSI_BOLD_OFF),
            [THEME_CODE_BLOCK] = ROLE_STYLE(ANSI_DIM, ANSI_BOLD_OFF),
            [THEME_HEADING]    = ROLE_STYLE(ANSI_BOLD, ANSI_BOLD_OFF),
            [THEME_LINK]       = ROLE_STYLE(ANSI_UNDERLINE, ANSI_UNDERLINE_OFF),
        },
    },
};

struct tint {
    const char *name;
    const char *dark_opens[THEME_ROLE_COUNT];
    const char *light_opens[THEME_ROLE_COUNT];
};

/* Keep tints away from saturated status colors, the gray axis, and the warm user accent. */
/* clang-format off */
static const struct tint TINTS[] = {
    {.name = "teal"},
    {
        .name = "violet",
        .dark_opens = {
            [THEME_STANCE]      = FG256(140),
            [THEME_CODE_INLINE] = FG256(140),
            [THEME_CODE_BLOCK]  = FG256(97),
            [THEME_HEADING]     = ANSI_BOLD FG256(140),
            [THEME_LINK]        = LINK256(97),
        },
        .light_opens = {
            [THEME_STANCE]      = FG256(97),
            [THEME_CODE_INLINE] = FG256(97),
            [THEME_CODE_BLOCK]  = FG256(140),
            [THEME_HEADING]     = ANSI_BOLD FG256(97),
            [THEME_LINK]        = LINK256(140),
        },
    },
    {
        .name = "rose",
        .dark_opens = {
            [THEME_STANCE]      = FG256(168),
            [THEME_CODE_INLINE] = FG256(168),
            [THEME_CODE_BLOCK]  = FG256(132),
            [THEME_HEADING]     = ANSI_BOLD FG256(168),
            [THEME_LINK]        = LINK256(132),
        },
        .light_opens = {
            [THEME_STANCE]      = FG256(132),
            [THEME_CODE_INLINE] = FG256(132),
            [THEME_CODE_BLOCK]  = FG256(168),
            [THEME_HEADING]     = ANSI_BOLD FG256(132),
            [THEME_LINK]        = LINK256(168),
        },
    },
    {
        .name = "sage",
        .dark_opens = {
            [THEME_STANCE]      = FG256(114),
            [THEME_CODE_INLINE] = FG256(114),
            [THEME_CODE_BLOCK]  = FG256(71),
            [THEME_HEADING]     = ANSI_BOLD FG256(114),
            [THEME_LINK]        = LINK256(71),
        },
        .light_opens = {
            [THEME_STANCE]      = FG256(71),
            [THEME_CODE_INLINE] = FG256(71),
            [THEME_CODE_BLOCK]  = FG256(114),
            [THEME_HEADING]     = ANSI_BOLD FG256(71),
            [THEME_LINK]        = LINK256(114),
        },
    },
};
/* clang-format on */

static const struct theme_preset *active_theme = &THEME_PRESETS[0];
static const struct tint *active_tint = &TINTS[0];

struct custom_theme {
    struct theme_preset preset;
    char *name;
    char *opens[THEME_ROLE_COUNT];
    char *closes[THEME_ROLE_COUNT];
};

static struct custom_theme custom_theme;

struct custom_tint {
    char *name;
    char *opens[THEME_ROLE_COUNT];
    char *closes[THEME_ROLE_COUNT];
};

static struct custom_tint custom_tint;

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

static const char *const *tint_role_opens(const struct tint *tint)
{
    switch (active_theme->tint_palette) {
    case TINT_PALETTE_DARK:
        return tint->dark_opens;
    case TINT_PALETTE_LIGHT:
        return tint->light_opens;
    case TINT_PALETTE_NONE:
        return NULL;
    }
    return NULL;
}

static const char *resolve_role_open(const char *const *tint_opens, enum theme_role role)
{
    if (tint_opens && tint_opens[role])
        return tint_opens[role];
    const char *open = active_theme->roles[role].open;
    return open ? open : "";
}

const char *theme_open(enum theme_role role)
{
    if (active_theme->tint_palette != TINT_PALETTE_NONE && custom_tint.opens[role])
        return custom_tint.opens[role];
    return resolve_role_open(tint_role_opens(active_tint), role);
}

const char *theme_close(enum theme_role role)
{
    if (active_theme->tint_palette != TINT_PALETTE_NONE && custom_tint.opens[role])
        return custom_tint.closes[role];
    const char *close = active_theme->roles[role].close;
    return close ? close : "";
}

const char *theme_name(void)
{
    return active_theme->name;
}

const char *theme_tint_name(void)
{
    return custom_tint.name ? custom_tint.name : active_tint->name;
}

const char *theme_tint_open(const char *name)
{
    if (active_theme->tint_palette != TINT_PALETTE_NONE && name && custom_tint.name &&
        strcasecmp(name, custom_tint.name) == 0)
        return custom_tint.opens[THEME_STANCE];
    const struct tint *tint = tint_find(name);
    if (!tint)
        return NULL;
    const char *const *tint_opens = tint_role_opens(tint);
    if (!tint_opens)
        return NULL;
    return resolve_role_open(tint_opens, THEME_STANCE);
}

/* COLORFGBG is the only common background hint that requires no terminal round trip. */
static const char *autodetect_theme(void)
{
    const char *no_color = getenv("NO_COLOR");
    if (no_color && *no_color)
        return "off";
    const char *term = getenv("TERM");
    if (!term || !*term || strcmp(term, "dumb") == 0)
        return "off";
    const char *colorterm = getenv("COLORTERM");
    if (!strstr(term, "256color") && !(colorterm && *colorterm))
        return "ansi";
    const char *color_fgbg = getenv("COLORFGBG");
    if (color_fgbg) {
        const char *background = strrchr(color_fgbg, ';');
        if (background && (strcmp(background + 1, "7") == 0 || strcmp(background + 1, "15") == 0))
            return "light";
    }
    return "dark";
}

static void custom_theme_free(struct custom_theme *theme)
{
    free(theme->name);
    for (int role = 0; role < THEME_ROLE_COUNT; role++) {
        free(theme->opens[role]);
        free(theme->closes[role]);
    }
    memset(theme, 0, sizeof(*theme));
}

static const struct theme_preset *builtin_theme(const char *name)
{
    for (size_t i = 0; i < sizeof(THEME_PRESETS) / sizeof(THEME_PRESETS[0]); i++)
        if (strcasecmp(THEME_PRESETS[i].name, name) == 0)
            return &THEME_PRESETS[i];
    return NULL;
}

static int color_code(json_t *value, int background, char *buffer, size_t size)
{
    const char *text = json_is_string(value) ? json_string_value(value) : NULL;
    long indexed = json_is_integer(value) ? json_integer_value(value) : -2;
    if (text && strcasecmp(text, "default") == 0) {
        snprintf(buffer, size, "%d", background ? 49 : 39);
        return 0;
    }
    if (text && text[0] == '#' && strlen(text) == 7) {
        for (size_t i = 1; i < 7; i++)
            if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f') ||
                  (text[i] >= 'A' && text[i] <= 'F')))
                return -1;
        char pair[3] = {0};
        pair[0] = text[1];
        pair[1] = text[2];
        int red = (int)strtol(pair, NULL, 16);
        pair[0] = text[3];
        pair[1] = text[4];
        int green = (int)strtol(pair, NULL, 16);
        pair[0] = text[5];
        pair[1] = text[6];
        int blue = (int)strtol(pair, NULL, 16);
        snprintf(buffer, size, "%d;2;%d;%d;%d", background ? 48 : 38, red, green, blue);
        return 0;
    }
    if (text) {
        static const struct {
            const char *name;
            int code;
        } NAMES[] = {{"black", 0},         {"red", 1},
                     {"green", 2},         {"yellow", 3},
                     {"blue", 4},          {"magenta", 5},
                     {"cyan", 6},          {"white", 7},
                     {"bright-black", 8},  {"bright-red", 9},
                     {"bright-green", 10}, {"bright-yellow", 11},
                     {"bright-blue", 12},  {"bright-magenta", 13},
                     {"bright-cyan", 14},  {"bright-white", 15}};
        for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
            if (strcasecmp(text, NAMES[i].name) != 0)
                continue;
            int code = NAMES[i].code;
            snprintf(buffer, size, "%d", (background ? 40 : 30) + code + (code >= 8 ? 52 : 0));
            return 0;
        }
        char *end;
        errno = 0;
        indexed = strtol(text, &end, 10);
        if (errno || *end)
            return -1;
    }
    if (indexed < 0 || indexed > 255)
        return -1;
    snprintf(buffer, size, "%d;5;%ld", background ? 48 : 38, indexed);
    return 0;
}

static int role_number(const char *name)
{
    static const char *const NAMES[] = {
        "accent", "chrome", "chrome_dim", "stance", "code_inline", "code_block", "heading",
        "link",   "add",    "remove",     "ok",     "error",       "warn"};
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
        if (strcmp(name, NAMES[i]) == 0)
            return (int)i;
    return -1;
}

static int append_sgr_code(char *codes, size_t size, const char *code)
{
    size_t length = strlen(codes);
    return snprintf(codes + length, size - length, "%s%s", length ? ";" : "", code) >=
                   (int)(size - length)
               ? -1
               : 0;
}

static int style_key_valid(const char *name)
{
    return strcmp(name, "fg") == 0 || strcmp(name, "bg") == 0 || strcmp(name, "bold") == 0 ||
           strcmp(name, "dim") == 0 || strcmp(name, "italic") == 0 ||
           strcmp(name, "underline") == 0 || strcmp(name, "reverse") == 0;
}

static int custom_role_style(json_t *value, char **open, char **close)
{
    json_t *fg = value;
    json_t *bg = NULL;
    int bold = 0;
    int dim = 0;
    int italic = 0;
    int underline = 0;
    int reverse = 0;
    if (json_is_object(value)) {
        const char *key;
        json_t *member;
        json_object_foreach(value, key, member)
        {
            if (!style_key_valid(key))
                return -1;
        }
        fg = json_object_get(value, "fg");
        bg = json_object_get(value, "bg");
#define STYLE_BOOL(key, target)                                                                    \
    do {                                                                                           \
        member = json_object_get(value, key);                                                      \
        if (member && !json_is_boolean(member))                                                    \
            return -1;                                                                             \
        (target) = json_is_true(member);                                                           \
    } while (0)
        STYLE_BOOL("bold", bold);
        STYLE_BOOL("dim", dim);
        STYLE_BOOL("italic", italic);
        STYLE_BOOL("underline", underline);
        STYLE_BOOL("reverse", reverse);
#undef STYLE_BOOL
        if (fg && !json_is_string(fg) && !json_is_integer(fg))
            return -1;
        if (bg && !json_is_string(bg) && !json_is_integer(bg))
            return -1;
    } else if (!json_is_string(value) && !json_is_integer(value)) {
        return -1;
    }

    char codes[64] = "";
    char color[24];
    if (bold && append_sgr_code(codes, sizeof(codes), "1"))
        return -1;
    if (dim && append_sgr_code(codes, sizeof(codes), "2"))
        return -1;
    if (italic && append_sgr_code(codes, sizeof(codes), "3"))
        return -1;
    if (underline && append_sgr_code(codes, sizeof(codes), "4"))
        return -1;
    if (reverse && append_sgr_code(codes, sizeof(codes), "7"))
        return -1;
    if (fg) {
        if (color_code(fg, 0, color, sizeof(color)) || append_sgr_code(codes, sizeof(codes), color))
            return -1;
    }
    if (bg) {
        if (color_code(bg, 1, color, sizeof(color)) || append_sgr_code(codes, sizeof(codes), color))
            return -1;
    }
    if (!codes[0])
        return -1;

    *open = xasprintf(ANSI_CSI "%sm", codes);
    char closes[24] = "";
    if (bold || dim)
        append_sgr_code(closes, sizeof(closes), "22");
    if (italic)
        append_sgr_code(closes, sizeof(closes), "23");
    if (underline)
        append_sgr_code(closes, sizeof(closes), "24");
    if (reverse)
        append_sgr_code(closes, sizeof(closes), "27");
    if (fg)
        append_sgr_code(closes, sizeof(closes), "39");
    if (bg)
        append_sgr_code(closes, sizeof(closes), "49");
    *close = closes[0] ? xasprintf(ANSI_CSI "%sm", closes) : xstrdup("");
    return 0;
}

static int custom_tint_roles_valid(json_t *roles)
{
    if (!json_is_object(roles))
        return -1;
    const char *role_name;
    json_t *value;
    json_object_foreach(roles, role_name, value)
    {
        int role = role_number(role_name);
        char *open = NULL;
        char *close = NULL;
        if (role < THEME_STANCE || role > THEME_LINK || custom_role_style(value, &open, &close)) {
            free(open);
            free(close);
            return -1;
        }
        free(open);
        free(close);
    }
    return 0;
}

static int custom_tints_valid(json_t *tints)
{
    if (!tints)
        return 0;
    if (!json_is_object(tints))
        return -1;
    const char *name;
    json_t *roles;
    json_object_foreach(tints, name, roles)
    {
        if (custom_tint_roles_valid(roles))
            return -1;
    }
    return 0;
}

static int custom_theme_load(const char *name, struct custom_theme *loaded)
{
    const json_t *themes = config_json_node("themes");
    json_t *definition = json_is_object(themes) ? json_object_get(themes, name) : NULL;
    if (!json_is_object(definition))
        return -1;

    json_t *extends_value = json_object_get(definition, "extends");
    if (extends_value && !json_is_string(extends_value))
        return -1;
    const char *extends = extends_value ? json_string_value(extends_value) : "dark";
    const struct theme_preset *base = builtin_theme(extends);
    if (!base || strcmp(base->name, "off") == 0)
        return -1;
    json_t *roles = json_object_get(definition, "roles");
    if (roles && !json_is_object(roles))
        return -1;
    if (custom_tints_valid(json_object_get(definition, "tints")))
        return -1;

    loaded->preset = *base;
    loaded->preset.name = loaded->name = xstrdup(name);
    if (!roles)
        return 0;
    const char *role_name;
    json_t *value;
    json_object_foreach(roles, role_name, value)
    {
        int role = role_number(role_name);
        if (role < 0 || custom_role_style(value, &loaded->opens[role], &loaded->closes[role]))
            return -1;
        loaded->preset.roles[role].open = loaded->opens[role];
        loaded->preset.roles[role].close = loaded->closes[role];
    }
    return 0;
}

int theme_name_valid(const char *name)
{
    if (!name)
        return 0;
    if (strcasecmp(name, "auto") == 0 || builtin_theme(name))
        return 1;
    struct custom_theme loaded = {0};
    int valid = custom_theme_load(name, &loaded) == 0;
    custom_theme_free(&loaded);
    return valid;
}

static void custom_tint_free(void)
{
    free(custom_tint.name);
    for (int role = 0; role < THEME_ROLE_COUNT; role++) {
        free(custom_tint.opens[role]);
        free(custom_tint.closes[role]);
    }
    memset(&custom_tint, 0, sizeof(custom_tint));
}

static json_t *custom_tint_roles(const char *name)
{
    if (!custom_theme.name)
        return NULL;
    const json_t *themes = config_json_node("themes");
    json_t *definition = json_is_object(themes) ? json_object_get(themes, custom_theme.name) : NULL;
    json_t *tints = json_is_object(definition) ? json_object_get(definition, "tints") : NULL;
    return json_is_object(tints) ? json_object_get(tints, name) : NULL;
}

static int custom_tint_load(const char *name)
{
    json_t *roles = custom_tint_roles(name);
    if (!json_is_object(roles))
        return -1;

    struct custom_tint loaded = {.name = xstrdup(name)};
    const char *role_name;
    json_t *value;
    json_object_foreach(roles, role_name, value)
    {
        int role = role_number(role_name);
        if (role < THEME_STANCE || role > THEME_LINK ||
            custom_role_style(value, &loaded.opens[role], &loaded.closes[role])) {
            free(loaded.name);
            for (int i = 0; i < THEME_ROLE_COUNT; i++) {
                free(loaded.opens[i]);
                free(loaded.closes[i]);
            }
            return -1;
        }
    }
    custom_tint_free();
    custom_tint = loaded;
    return 0;
}

int theme_tint_valid(const char *name)
{
    if (!name)
        return 0;
    if (tint_find(name))
        return 1;
    return custom_tint_roles_valid(custom_tint_roles(name)) == 0;
}

int theme_set(const char *name)
{
    if (!name)
        return -1;
    if (strcasecmp(name, "auto") == 0)
        name = autodetect_theme();
    const struct theme_preset *builtin = builtin_theme(name);
    if (builtin) {
        custom_tint_free();
        custom_theme_free(&custom_theme);
        active_theme = builtin;
        return 0;
    }

    struct custom_theme loaded = {0};
    if (custom_theme_load(name, &loaded) != 0) {
        custom_theme_free(&loaded);
        return -1;
    }
    custom_tint_free();
    custom_theme_free(&custom_theme);
    custom_theme = loaded;
    active_theme = &custom_theme.preset;
    return 0;
}

int theme_tint_set(const char *name)
{
    if (!name)
        return -1;
    if (custom_tint_load(name) == 0)
        return 0;
    const struct tint *tint = tint_find(name);
    if (!tint)
        return -1;
    custom_tint_free();
    active_tint = tint;
    return 0;
}

static int invalid_value_changed(char **last_value, const char *value)
{
    if (*last_value && strcmp(*last_value, value) == 0)
        return 0;
    free(*last_value);
    *last_value = xstrdup(value);
    return 1;
}

/* A runtime tint overrides the active preset's tint; lower tiers apply when the preset has none. */
static const char *configured_tint(void)
{
    const char *tint = NULL;
    if (strcmp(config_source("tint"), "run") == 0)
        tint = config_str("tint");
    if (!tint || !*tint) {
        const char *preset = config_str("preset");
        tint = (preset && *preset) ? config_preset_tint(preset) : NULL;
    }
    if (!tint || !*tint)
        tint = config_str("tint");
    return (tint && *tint) ? tint : "teal";
}

void theme_init(void)
{
    static char *last_invalid_theme;
    static char *last_invalid_tint;

    const char *theme = config_str("theme");
    if (!theme)
        theme = "auto";
    if (theme_set(theme) != 0) {
        theme_set("auto");
        if (invalid_value_changed(&last_invalid_theme, theme))
            hax_warn("unknown theme '%s' (expected auto, dark, light, ansi, off, or themes.<name>)",
                     theme);
    }

    const char *tint = configured_tint();
    if (theme_tint_set(tint) != 0) {
        theme_tint_set("teal");
        if (invalid_value_changed(&last_invalid_tint, tint))
            hax_warn(
                "unknown tint '%s' (expected teal, violet, rose, sage, or an active theme tint)",
                tint);
    }
}
