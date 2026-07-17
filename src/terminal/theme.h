/* SPDX-License-Identifier: MIT */
#ifndef HAX_THEME_H
#define HAX_THEME_H

/* Semantic color roles, resolved to concrete SGR sequences by the active
 * theme preset. Call sites name the meaning (accent, error, ...) and the
 * preset decides how it looks, so palettes can change without touching
 * renderers — and per-role user overrides stay possible later.
 *
 * Scope: these are the *color* roles. Bold / italic / dim remain plain
 * ANSI attributes (ansi.h) at call sites: several spots compose them
 * around role colors (e.g. the dim arg after a quiet tool tag, or SGR
 * 22 closing dim while an outer color span stays open), which only
 * works while they are attributes, not foreground colors.
 *
 * THEME_CHROME_DIM owns its full quiet styling: SGR 2 (faint) over an
 * indexed 256-color foreground is inconsistently implemented (some
 * terminals faint it, some ignore it), so the rich presets encode the
 * quiet look purely in the color while the ansi/off presets carry the
 * dim attribute themselves. Call sites must not wrap it in ANSI_DIM;
 * they still emit ANSI_DIM for adjacent runs on the default
 * foreground, where faint is portable.
 *
 * theme_close(role) undoes exactly what theme_open(role) set (foreground
 * and/or intensity) and leaves other live attributes intact, mirroring
 * ansi.h's per-attribute closers. ANSI_RESET remains the blunt closer. */

enum theme_role {
    THEME_ACCENT,      /* user identity: prompt marker, input stripe, picker arrow */
    THEME_CHROME,      /* app frame: banner bar, loud tool tags, help listings */
    THEME_CHROME_DIM,  /* quiet chrome: gutter strips, silent/collapsed tool tags */
    THEME_CODE_INLINE, /* markdown `code` spans */
    THEME_CODE_BLOCK,  /* markdown ``` fence bodies */
    THEME_HEADING,     /* markdown headings (may include bold) */
    THEME_ADD,         /* diff added lines */
    THEME_REMOVE,      /* diff removed lines */
    THEME_OK,          /* positive status tags */
    THEME_ERROR,       /* error diagnostics */
    THEME_WARN,        /* warning diagnostics */
    THEME_ROLE_COUNT,
};

/* Open/close sequences for a role under the active preset. Never NULL;
 * "" when the preset styles nothing (theme off). The returned pointers
 * are static — safe to cache within one preset, but theme_set/theme_init
 * invalidate the association, so prefer looking up per use. */
const char *theme_open(enum theme_role role);
const char *theme_close(enum theme_role role);

/* Activate a preset by name: auto, dark, light, ansi, off. "auto" picks
 * from the terminal environment (NO_COLOR, TERM, COLORTERM, COLORFGBG).
 * Returns 0, or -1 for an unknown name (active preset unchanged).
 * Until the first call the "ansi" preset is active — the pre-theme
 * palette, so library-style use and tests need no setup. */
int theme_set(const char *name);

/* Resolve the "theme" config setting and activate it. Call once at
 * startup after config_init(); warns and falls back to auto on an
 * unknown name. */
void theme_init(void);

/* Name of the active preset (after auto-detection: the concrete pick). */
const char *theme_name(void);

#endif /* HAX_THEME_H */
