/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_GLYPHS_H
#define HAX_TERMINAL_GLYPHS_H

#include <stddef.h>

/* Configured glyphs are one terminal cell and no more than this many UTF-8 bytes. */
#define GLYPH_MAX_BYTES 16

enum glyph {
    GLYPH_PROMPT,
    GLYPH_USER_GUTTER,
    GLYPH_BANNER_GUTTER,
    GLYPH_SEPARATOR,
    GLYPH_TOOL_FIRST,
    GLYPH_TOOL_BODY,
    GLYPH_TOOL_LAST,
    GLYPH_TOOL_MARKER,
    GLYPH_PICKER_SEARCH,
    GLYPH_PICKER_SELECTED,
    GLYPH_PICKER_CURRENT,
    GLYPH_COUNT,
};

/* Return whether name selects auto, a shipped glyph theme, or a glyph_themes.<name> definition. */
int glyph_theme_name_valid(const char *name);

/* Load the selected glyph theme. Omitted marks inherit shipped UTF-8 glyphs. */
void glyphs_init(void);

/* Return a static, printable one-cell glyph selected for the active locale. */
const char *glyph(enum glyph glyph);

/* Return the configured animation frame and frame count. Frames are one terminal cell. */
const char *glyph_spinner_frame(size_t frame);
size_t glyph_spinner_frame_count(void);

#endif /* HAX_TERMINAL_GLYPHS_H */
