/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"
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

/* All spinner entry points must be silent no-ops on the NULL spinner non-TTY runs carry. */
static void test_spinner_is_null_and_silent_without_tty(void)
{
    capture_reset();
    struct spinner *spinner = spinner_new(NULL);
    EXPECT(spinner == NULL);

    spinner_show(spinner);
    spinner_set_label(spinner, "thinking", "thinking...");
    spinner_request_label(spinner, "reading", "reading...");
    spinner_set_timer(spinner, 1);
    spinner_park(spinner, 4);
    struct spinner_row row = {.bytes = "| live output", .cells = 13};
    spinner_set_tool_status_view(spinner, &row, 1);
    spinner_swap_begin(spinner);
    spinner_swap_end(spinner);
    spinner_hide(spinner);
    spinner_free(spinner);
    EXPECT_STR_EQ(capture_read(), "");
}

#define FRAME_CHROME ANSI_DIM ANSI_CYAN

static void test_tool_frame_paints_rows_verbatim(void)
{
    struct buf frame;
    buf_init(&frame);
    struct spinner_tool_frame painted;
    struct spinner_row rows[] = {{.bytes = "<older>", .cells = 7},
                                 {.bytes = "<newest>", .cells = 8}};

    spinner_build_tool_frame(&frame, rows, 2, "*", 80, NULL, &painted);
    EXPECT_STR_EQ(frame.data, ANSI_SYNC_BEGIN "\r"
                                              "<older>" ANSI_ERASE_LINE "\r\n"
                                              "<newest>" ANSI_ERASE_BELOW "\r" FRAME_CHROME
                                              "*" ANSI_RESET ANSI_SYNC_END);
    EXPECT(painted.row_count == 2);
    EXPECT(painted.row_widths[0] == 7);
    EXPECT(painted.row_widths[1] == 8);
    buf_free(&frame);
}

static void test_tool_frame_climb_accounts_for_reflow(void)
{
    struct buf frame;
    buf_init(&frame);
    struct spinner_tool_frame painted;
    struct spinner_tool_frame previous = {.row_widths = {100, 10}, .row_count = 2};
    struct spinner_row rows[] = {{.bytes = "a", .cells = 3}};

    /* The 100-cell row wraps to three physical rows at 40 columns. */
    spinner_build_tool_frame(&frame, rows, 1, "*", 40, &previous, &painted);
    EXPECT(strstr(frame.data, "\x1b[3A") != NULL);
    EXPECT(painted.row_count == 1);
    EXPECT(painted.row_widths[0] == 3);
    buf_free(&frame);
}

/* The cursor rests on the last logical row's first physical row, so a wrapped last row must not
 * contribute to the climb; over-climbing would erase settled output above the live block. */
static void test_tool_frame_climb_excludes_wrapped_last_row(void)
{
    struct buf frame;
    buf_init(&frame);
    struct spinner_tool_frame painted;
    struct spinner_tool_frame previous = {.row_widths = {100, 90}, .row_count = 2};
    struct spinner_row rows[] = {{.bytes = "a", .cells = 3}};

    /* Only the preceding 100-cell row (three physical rows at 40 columns) is climbed; the
     * wrapped 90-cell last row lies below the cursor. */
    spinner_build_tool_frame(&frame, rows, 1, "*", 40, &previous, &painted);
    EXPECT(strstr(frame.data, "\x1b[3A") != NULL);
    EXPECT(strstr(frame.data, "\x1b[5A") == NULL);
    buf_free(&frame);

    /* A lone wrapped row needs no climb at all; nothing else in the frame emits cursor-up. */
    struct buf solo;
    buf_init(&solo);
    struct spinner_tool_frame previous_solo = {.row_widths = {100}, .row_count = 1};
    spinner_build_tool_frame(&solo, rows, 1, "*", 40, &previous_solo, &painted);
    EXPECT(strchr(solo.data, 'A') == NULL);
    buf_free(&solo);
}

int main(void)
{
    capture_init();
    test_glyph_is_one_braille_codepoint();
    test_spinner_is_null_and_silent_without_tty();
    test_tool_frame_paints_rows_verbatim();
    test_tool_frame_climb_accounts_for_reflow();
    test_tool_frame_climb_excludes_wrapped_last_row();
    T_REPORT();
}
