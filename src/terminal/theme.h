/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_THEME_H
#define HAX_TERMINAL_THEME_H

/* Semantic SGR roles. THEME_CHROME_DIM includes its intensity styling; callers must not add
 * ANSI_DIM. Other bold, italic, and dim styling remains explicit at the call site. */
enum theme_role {
    THEME_ACCENT,      /* user identity: prompts, input, and picker selection */
    THEME_CHROME,      /* application frame and prominent labels */
    THEME_CHROME_DIM,  /* quiet application frame and labels */
    THEME_STANCE,      /* active preset name */
    THEME_CODE_INLINE, /* Markdown inline code */
    THEME_CODE_BLOCK,  /* Markdown fenced code */
    THEME_HEADING,     /* Markdown headings; may include bold */
    THEME_LINK,        /* Markdown bare URLs; includes underline */
    THEME_ADD,         /* added diff lines */
    THEME_REMOVE,      /* removed diff lines */
    THEME_OK,          /* positive status */
    THEME_ERROR,       /* error diagnostics */
    THEME_WARN,        /* warning diagnostics */
    THEME_ROLE_COUNT,
};

/* Return static open and close sequences for a valid role. The sequences are never NULL, and the
 * close sequence reverses only attributes set by the corresponding open sequence. Theme and tint
 * changes may change which static sequence is returned. */
const char *theme_open(enum theme_role role);
const char *theme_close(enum theme_role role);

/* Select auto, dark, light, ansi, or off by case-insensitive name. "auto" inspects NO_COLOR, TERM,
 * COLORTERM, and COLORFGBG. The initial theme is "ansi". Return 0 on success or -1 without
 * changing the current theme. */
int theme_set(const char *name);

/* Select teal, violet, rose, or sage by case-insensitive name. Tints affect only the stance and
 * Markdown roles, and do not apply to the ansi or off themes. The initial tint is "teal". Return 0
 * on success or -1 without changing the current tint. */
int theme_tint_set(const char *name);

/* Apply the configured theme and tint. An invalid value falls back to "auto" or "teal"; repeated
 * resolution of the same invalid value does not repeat its warning. */
void theme_init(void);

const char *theme_name(void);
const char *theme_tint_name(void);

/* Return the named tint's static stance-opening sequence without activating it. Return NULL for an
 * unknown tint or when the active theme does not apply tints. Close with
 * theme_close(THEME_STANCE). */
const char *theme_tint_open(const char *name);

#endif /* HAX_TERMINAL_THEME_H */
