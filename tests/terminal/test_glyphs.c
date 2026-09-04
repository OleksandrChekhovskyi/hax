/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "config.h"
#include "harness.h"
#include "system/locale.h"
#include "terminal/glyphs.h"

static void test_defaults(void)
{
    glyphs_init();
    EXPECT_STR_EQ(glyph(GLYPH_PROMPT), "❯");
    EXPECT(glyph_spinner_frame_count() == 10);
    EXPECT_STR_EQ(glyph_spinner_frame(0), "⠋");
}

static void test_builtin_ascii_theme(void)
{
    EXPECT(config_load("{\"glyph_theme\": \"ascii\"}") == 0);
    glyphs_init();
    EXPECT_STR_EQ(glyph(GLYPH_PROMPT), ">");
    EXPECT_STR_EQ(glyph(GLYPH_PICKER_CURRENT), "*");
    EXPECT(glyph_spinner_frame_count() == 4);
    EXPECT_STR_EQ(glyph_spinner_frame(1), "\\");
    EXPECT(config_load(NULL) == 0);
    glyphs_init();
}

static void test_custom_glyph_theme(void)
{
    EXPECT(config_load("{\"glyph_theme\": \"custom\", \"glyph_themes\": {\"custom\": {"
                       "\"prompt\": \"$\", \"user_gutter\": \"!\","
                       " \"spinner\": [\".\", \"o\"]}, \"other\": {\"prompt\": \"?\"}}}") == 0);
    const struct config_setting *glyph_theme = config_setting_find("glyph_theme");
    EXPECT(config_value_valid(glyph_theme, "custom"));
    EXPECT(!config_value_valid(glyph_theme, "missing"));
    glyphs_init();
    EXPECT_STR_EQ(glyph(GLYPH_PROMPT), "$");
    EXPECT_STR_EQ(glyph(GLYPH_USER_GUTTER), "!");
    EXPECT(glyph_spinner_frame_count() == 2);
    EXPECT_STR_EQ(glyph_spinner_frame(0), ".");
    EXPECT_STR_EQ(glyph_spinner_frame(1), "o");
    EXPECT_STR_EQ(glyph_spinner_frame(2), ".");
    config_set_override("glyph_theme", "other");
    glyphs_init();
    EXPECT_STR_EQ(glyph(GLYPH_PROMPT), "?");
    config_set_override("glyph_theme", NULL);
    EXPECT(config_load(NULL) == 0);
    glyphs_init();
}

static void test_invalid_glyph_theme(void)
{
    EXPECT(config_load("{\"glyph_theme\": \"custom\", \"glyph_themes\": {\"custom\": {"
                       "\"tool_gutter\": \"+\", \"picker\": {\"unknown\": \"*\"}}}}") == 0);
    const struct config_setting *glyph_theme = config_setting_find("glyph_theme");
    EXPECT(!config_value_valid(glyph_theme, "custom"));
    glyphs_init();
    EXPECT_STR_EQ(glyph(GLYPH_PROMPT), "❯");
    EXPECT(config_load(NULL) == 0);
    glyphs_init();
}

static void test_glyph_byte_limit(void)
{
    EXPECT(config_load("{\"glyph_theme\": \"custom\", \"glyph_themes\": {\"custom\": {\"prompt\":"
                       " \".\\u0301\\u0301\\u0301\\u0301\\u0301\\u0301\\u0301\\u0301\"}}}") == 0);
    glyphs_init();
    EXPECT_STR_EQ(glyph(GLYPH_PROMPT), "❯");
    EXPECT(config_load(NULL) == 0);
    glyphs_init();
}

int main(void)
{
    locale_init_utf8();
    test_defaults();
    test_builtin_ascii_theme();
    test_custom_glyph_theme();
    test_invalid_glyph_theme();
    test_glyph_byte_limit();
    T_REPORT();
}
