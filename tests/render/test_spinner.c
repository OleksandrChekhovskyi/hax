/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harness.h"
#include "util.h"
#include "render/spinner.h"
#include "terminal/ansi.h"

#define BRAILLE_PREFIX "\xE2\xA0"

static char capture_buf[4096];

static void capture_init(void)
{
    locale_init_utf8();
    char *path = xasprintf("%s/stdout", t_tempdir());
    if (!freopen(path, "w+", stdout)) {
        perror("freopen");
        exit(1);
    }
    free(path);
}

static void capture_reset(void)
{
    fflush(stdout);
    if (ftruncate(fileno(stdout), 0) != 0) {
        perror("ftruncate");
        exit(1);
    }
    rewind(stdout);
}

static const char *capture_read(void)
{
    fflush(stdout);
    rewind(stdout);
    size_t length = fread(capture_buf, 1, sizeof(capture_buf) - 1, stdout);
    capture_buf[length] = '\0';
    return capture_buf;
}

static void test_glyph_is_one_braille_codepoint(void)
{
    const char *glyph = spinner_glyph_now();
    EXPECT(strncmp(glyph, BRAILLE_PREFIX, 2) == 0);
    EXPECT(glyph[3] == '\0');
}

static void test_label_spinner_is_silent_without_tty(void)
{
    capture_reset();
    struct spinner *spinner = spinner_new(NULL);
    spinner_show(spinner);
    spinner_set_label(spinner, "thinking", "thinking...");
    spinner_park(spinner, 4);
    spinner_hide(spinner);
    spinner_free(spinner);
    EXPECT_STR_EQ(capture_read(), "");
}

static void test_tool_status_paints_synchronously_without_tty(void)
{
    capture_reset();
    struct spinner *spinner = spinner_new(NULL);
    spinner_show_tool_status(spinner, "live output");

    const char *output = capture_read();
    EXPECT(strstr(output, BRAILLE_PREFIX) != NULL);
    EXPECT(strstr(output, "live output") != NULL);

    capture_reset();
    spinner_set_tool_status_content(spinner, "next output");
    EXPECT_STR_EQ(capture_read(), "");

    spinner_hide(spinner);
    EXPECT_STR_EQ(capture_read(), "\r" ANSI_ERASE_LINE);
    spinner_free(spinner);
}

static void test_free_erases_visible_tool_status(void)
{
    struct spinner *spinner = spinner_new(NULL);
    spinner_show_tool_status(spinner, "live output");
    capture_reset();

    spinner_free(spinner);
    EXPECT_STR_EQ(capture_read(), "\r" ANSI_ERASE_LINE);
}

int main(void)
{
    capture_init();
    test_glyph_is_one_braille_codepoint();
    test_label_spinner_is_silent_without_tty();
    test_tool_status_paints_synchronously_without_tty();
    test_free_erases_visible_tool_status();
    T_REPORT();
}
