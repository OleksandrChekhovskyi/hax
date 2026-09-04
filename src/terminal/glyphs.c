/* SPDX-License-Identifier: MIT */
#include "terminal/glyphs.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "diag.h"
#include "xalloc.h"
#include "system/locale.h"
#include "text/utf8.h"
#include "text/width.h"

static const char *const UTF8_GLYPHS[GLYPH_COUNT] = {
    [GLYPH_PROMPT] = "❯",          [GLYPH_USER_GUTTER] = "▌",    [GLYPH_BANNER_GUTTER] = "▌",
    [GLYPH_SEPARATOR] = "·",       [GLYPH_TOOL_FIRST] = "┌",     [GLYPH_TOOL_BODY] = "│",
    [GLYPH_TOOL_LAST] = "└",       [GLYPH_TOOL_MARKER] = "›",    [GLYPH_PICKER_SEARCH] = "⌕",
    [GLYPH_PICKER_SELECTED] = "→", [GLYPH_PICKER_CURRENT] = "✓",
};

static const char *const ASCII_GLYPHS[GLYPH_COUNT] = {
    [GLYPH_PROMPT] = ">",          [GLYPH_USER_GUTTER] = "|",    [GLYPH_BANNER_GUTTER] = "|",
    [GLYPH_SEPARATOR] = "|",       [GLYPH_TOOL_FIRST] = "+",     [GLYPH_TOOL_BODY] = "|",
    [GLYPH_TOOL_LAST] = "+",       [GLYPH_TOOL_MARKER] = ">",    [GLYPH_PICKER_SEARCH] = "/",
    [GLYPH_PICKER_SELECTED] = ">", [GLYPH_PICKER_CURRENT] = "*",
};

static const char *const UTF8_SPINNER[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
static const char *const ASCII_SPINNER[] = {"-", "\\", "|", "/"};

struct builtin_glyph_theme {
    const char *name;
    const char *const *glyphs;
    const char *const *spinner;
    size_t spinner_count;
};

static const struct builtin_glyph_theme BUILTIN_GLYPH_THEMES[] = {
    {.name = "utf8",
     .glyphs = UTF8_GLYPHS,
     .spinner = UTF8_SPINNER,
     .spinner_count = sizeof(UTF8_SPINNER) / sizeof(UTF8_SPINNER[0])},
    {.name = "ascii",
     .glyphs = ASCII_GLYPHS,
     .spinner = ASCII_SPINNER,
     .spinner_count = sizeof(ASCII_SPINNER) / sizeof(ASCII_SPINNER[0])},
};

struct glyph_entry {
    enum glyph glyph;
    const char *name;
};

static const struct glyph_entry GLYPH_ENTRIES[] = {
    {GLYPH_PROMPT, "prompt"},
    {GLYPH_USER_GUTTER, "user_gutter"},
    {GLYPH_BANNER_GUTTER, "banner_gutter"},
    {GLYPH_SEPARATOR, "separator"},
};

static const struct glyph_entry TOOL_GUTTER_ENTRIES[] = {
    {GLYPH_TOOL_FIRST, "first"},
    {GLYPH_TOOL_BODY, "body"},
    {GLYPH_TOOL_LAST, "last"},
    {GLYPH_TOOL_MARKER, "marker"},
};

static const struct glyph_entry PICKER_ENTRIES[] = {
    {GLYPH_PICKER_SEARCH, "search"},
    {GLYPH_PICKER_SELECTED, "selected"},
    {GLYPH_PICKER_CURRENT, "current"},
};

static const char *active_glyphs[GLYPH_COUNT];
static char *glyph_overrides[GLYPH_COUNT];
static char **spinner_frames;
static size_t spinner_count;

static const struct builtin_glyph_theme *builtin_glyph_theme(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof(BUILTIN_GLYPH_THEMES) / sizeof(BUILTIN_GLYPH_THEMES[0]); i++) {
        if (strcasecmp(BUILTIN_GLYPH_THEMES[i].name, name) == 0)
            return &BUILTIN_GLYPH_THEMES[i];
    }
    return NULL;
}

static int glyph_text_valid(const char *text)
{
    size_t length = text ? strlen(text) : 0;
    if (!length || length > GLYPH_MAX_BYTES || !utf8_is_valid(text, length) ||
        display_cells(text) != 1)
        return 0;

    for (size_t offset = 0; offset < length;) {
        size_t bytes;
        if (utf8_codepoint_cells(text, length, offset, &bytes) < 0)
            return 0;
        offset += bytes;
    }
    return 1;
}

static void free_spinner(void)
{
    for (size_t i = 0; i < spinner_count; i++)
        free(spinner_frames[i]);
    free(spinner_frames);
    spinner_frames = NULL;
    spinner_count = 0;
}

static void set_spinner(const char *const *frames, size_t count)
{
    free_spinner();
    spinner_frames = xcalloc(count, sizeof(*spinner_frames));
    spinner_count = count;
    for (size_t i = 0; i < count; i++)
        spinner_frames[i] = xstrdup(frames[i]);
}

static void select_builtin_theme(const struct builtin_glyph_theme *theme)
{
    for (int i = 0; i < GLYPH_COUNT; i++) {
        free(glyph_overrides[i]);
        glyph_overrides[i] = NULL;
        active_glyphs[i] = theme->glyphs[i];
    }
    set_spinner(theme->spinner, theme->spinner_count);
}

static int load_glyph(enum glyph glyph, const char *name, json_t *node)
{
    const char *text = json_is_string(node) ? json_string_value(node) : NULL;
    if (!text || !glyph_text_valid(text)) {
        hax_warn("glyph theme %s must be one printable terminal cell", name);
        return -1;
    }
    glyph_overrides[glyph] = xstrdup(text);
    active_glyphs[glyph] = glyph_overrides[glyph];
    return 0;
}

static void load_spinner(json_t *node)
{
    if (!json_is_array(node) || json_array_size(node) == 0) {
        hax_warn("glyph theme spinner must be a nonempty array");
        return;
    }

    size_t count = json_array_size(node);
    char **frames = xcalloc(count, sizeof(*frames));
    for (size_t i = 0; i < count; i++) {
        const char *frame = json_is_string(json_array_get(node, i))
                                ? json_string_value(json_array_get(node, i))
                                : NULL;
        if (!frame || !glyph_text_valid(frame)) {
            hax_warn("glyph theme spinner[%zu] must be one printable terminal cell", i);
            for (size_t j = 0; j < i; j++)
                free(frames[j]);
            free(frames);
            return;
        }
        frames[i] = xstrdup(frame);
    }

    free_spinner();
    spinner_frames = frames;
    spinner_count = count;
}

static void load_entries(json_t *object, const struct glyph_entry *entries, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        json_t *node = json_object_get(object, entries[i].name);
        if (node)
            load_glyph(entries[i].glyph, entries[i].name, node);
    }
}

static void load_glyphs(json_t *glyphs)
{
    load_entries(glyphs, GLYPH_ENTRIES, sizeof(GLYPH_ENTRIES) / sizeof(GLYPH_ENTRIES[0]));
    json_t *tool_gutter = json_object_get(glyphs, "tool_gutter");
    if (tool_gutter)
        load_entries(tool_gutter, TOOL_GUTTER_ENTRIES,
                     sizeof(TOOL_GUTTER_ENTRIES) / sizeof(TOOL_GUTTER_ENTRIES[0]));
    json_t *picker = json_object_get(glyphs, "picker");
    if (picker)
        load_entries(picker, PICKER_ENTRIES, sizeof(PICKER_ENTRIES) / sizeof(PICKER_ENTRIES[0]));
    json_t *spinner = json_object_get(glyphs, "spinner");
    if (spinner)
        load_spinner(spinner);
}

static int entries_valid(json_t *object, const struct glyph_entry *entries, size_t count)
{
    const char *name;
    json_t *value;
    json_object_foreach(object, name, value)
    {
        size_t i;
        for (i = 0; i < count; i++)
            if (strcmp(name, entries[i].name) == 0)
                break;
        if (i == count || !json_is_string(value) || !glyph_text_valid(json_string_value(value)))
            return 0;
    }
    return 1;
}

static int spinner_valid(json_t *spinner)
{
    if (!spinner)
        return 1;
    if (!json_is_array(spinner) || json_array_size(spinner) == 0)
        return 0;
    for (size_t i = 0; i < json_array_size(spinner); i++) {
        json_t *frame = json_array_get(spinner, i);
        if (!json_is_string(frame) || !glyph_text_valid(json_string_value(frame)))
            return 0;
    }
    return 1;
}

static int custom_glyph_theme_valid(json_t *glyphs)
{
    if (!json_is_object(glyphs))
        return 0;
    const char *name;
    json_t *value;
    json_object_foreach(glyphs, name, value)
    {
        if (strcmp(name, "spinner") == 0) {
            if (!spinner_valid(value))
                return 0;
        } else if (strcmp(name, "tool_gutter") == 0) {
            if (!json_is_object(value) ||
                !entries_valid(value, TOOL_GUTTER_ENTRIES,
                               sizeof(TOOL_GUTTER_ENTRIES) / sizeof(TOOL_GUTTER_ENTRIES[0])))
                return 0;
        } else if (strcmp(name, "picker") == 0) {
            if (!json_is_object(value) ||
                !entries_valid(value, PICKER_ENTRIES,
                               sizeof(PICKER_ENTRIES) / sizeof(PICKER_ENTRIES[0])))
                return 0;
        } else if (!json_is_string(value) || !glyph_text_valid(json_string_value(value))) {
            return 0;
        } else {
            size_t i;
            for (i = 0; i < sizeof(GLYPH_ENTRIES) / sizeof(GLYPH_ENTRIES[0]); i++)
                if (strcmp(name, GLYPH_ENTRIES[i].name) == 0)
                    break;
            if (i == sizeof(GLYPH_ENTRIES) / sizeof(GLYPH_ENTRIES[0]))
                return 0;
        }
    }
    return 1;
}

static json_t *custom_glyph_theme(const char *name)
{
    const json_t *themes = config_json_node("glyph_themes");
    return json_is_object(themes) ? json_object_get(themes, name) : NULL;
}

int glyph_theme_name_valid(const char *name)
{
    return name && (strcasecmp(name, "auto") == 0 || builtin_glyph_theme(name) ||
                    custom_glyph_theme_valid(custom_glyph_theme(name)));
}

static int glyph_theme_set(const char *name)
{
    if (!name)
        return -1;
    if (strcasecmp(name, "auto") == 0)
        name = locale_have_utf8() ? "utf8" : "ascii";
    const struct builtin_glyph_theme *builtin = builtin_glyph_theme(name);
    if (builtin) {
        select_builtin_theme(builtin);
        return 0;
    }
    json_t *custom = custom_glyph_theme(name);
    if (!custom_glyph_theme_valid(custom))
        return -1;
    select_builtin_theme(&BUILTIN_GLYPH_THEMES[0]);
    load_glyphs(custom);
    return 0;
}

void glyphs_init(void)
{
    const char *name = config_str("glyph_theme");
    if (glyph_theme_set(name ? name : "auto") == 0)
        return;
    glyph_theme_set("auto");
    hax_warn("unknown glyph theme '%s' (expected auto, utf8, ascii, or glyph_themes.<name>)",
             name ? name : "");
}

const char *glyph(enum glyph glyph)
{
    if (glyph < 0 || glyph >= GLYPH_COUNT)
        return "?";
    return active_glyphs[glyph] ? active_glyphs[glyph] : UTF8_GLYPHS[glyph];
}

const char *glyph_spinner_frame(size_t frame)
{
    if (!spinner_count)
        glyph_theme_set("auto");
    return spinner_frames[frame % spinner_count];
}

size_t glyph_spinner_frame_count(void)
{
    if (!spinner_count)
        glyph_theme_set("auto");
    return spinner_count;
}
