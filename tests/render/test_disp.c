/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "render/disp.h"

struct capture {
    struct disp disp;
    FILE *stream;
    char *bytes;
    size_t len;
};

static int capture_init(struct capture *capture, size_t committed_newlines)
{
    memset(capture, 0, sizeof(*capture));
    capture->stream = open_memstream(&capture->bytes, &capture->len);
    EXPECT(capture->stream != NULL);
    if (!capture->stream)
        return 0;
    capture->disp.sink = capture->stream;
    capture->disp.committed_newlines = committed_newlines;
    return 1;
}

static const char *capture_read(struct capture *capture)
{
    disp_flush(&capture->disp);
    return capture->bytes ? capture->bytes : "";
}

static void capture_free(struct capture *capture)
{
    fclose(capture->stream);
    free(capture->bytes);
}

static void test_putc_writes_visible_bytes(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_putc(&capture.disp, 'h');
    disp_putc(&capture.disp, 'i');

    EXPECT_STR_EQ(capture_read(&capture), "hi");
    EXPECT(capture.disp.committed_newlines == 0);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_putc_defers_trailing_newlines(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_putc(&capture.disp, 'a');
    disp_putc(&capture.disp, '\n');
    disp_putc(&capture.disp, '\n');

    EXPECT_STR_EQ(capture_read(&capture), "a");
    EXPECT(capture.disp.pending_newlines == 2);

    disp_commit_newlines(&capture.disp);
    EXPECT_STR_EQ(capture_read(&capture), "a\n\n");
    EXPECT(capture.disp.pending_newlines == 0);
    EXPECT(capture.disp.committed_newlines == 2);
    capture_free(&capture);
}

static void test_visible_byte_commits_pending_newlines(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "a\n", 2);
    disp_putc(&capture.disp, 'b');

    EXPECT_STR_EQ(capture_read(&capture), "a\nb");
    EXPECT(capture.disp.committed_newlines == 0);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_write_defers_trailing_newlines(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "hi\n\n", 4);

    EXPECT_STR_EQ(capture_read(&capture), "hi");
    EXPECT(capture.disp.pending_newlines == 2);
    capture_free(&capture);
}

static void test_write_normalizes_trailing_crlf(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "hi\r\n", 4);
    disp_commit_newlines(&capture.disp);

    EXPECT_STR_EQ(capture_read(&capture), "hi\n");
    capture_free(&capture);
}

static void test_write_preserves_trailing_carriage_return(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "hi\r", 3);

    EXPECT_STR_EQ(capture_read(&capture), "hi\r");
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_putc_carriage_return_preserves_newline_state(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "row\n", 4);
    disp_putc(&capture.disp, '\r');
    disp_block_separator(&capture.disp);

    EXPECT_STR_EQ(capture_read(&capture), "row\n\r\n");
    EXPECT(capture.disp.committed_newlines == 2);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_write_carriage_return_preserves_newline_state(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "row\n\r", 5);
    disp_block_separator(&capture.disp);

    EXPECT_STR_EQ(capture_read(&capture), "row\n\r\n");
    EXPECT(capture.disp.committed_newlines == 2);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_empty_write_is_noop(void)
{
    struct capture capture;
    if (!capture_init(&capture, 1))
        return;

    disp_write(&capture.disp, "", 0);

    EXPECT_STR_EQ(capture_read(&capture), "");
    EXPECT(capture.disp.committed_newlines == 1);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_block_separator_starts_with_blank_line(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_block_separator(&capture.disp);

    EXPECT_STR_EQ(capture_read(&capture), "\n\n");
    EXPECT(capture.disp.committed_newlines == 2);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_block_separator_collapses_pending_newlines(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "\n\n\n\n\n", 5);
    disp_block_separator(&capture.disp);

    EXPECT_STR_EQ(capture_read(&capture), "\n\n");
    EXPECT(capture.disp.committed_newlines == 2);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_block_separator_preserves_existing_blank_line(void)
{
    struct capture capture;
    if (!capture_init(&capture, 2))
        return;

    disp_block_separator(&capture.disp);

    EXPECT_STR_EQ(capture_read(&capture), "");
    EXPECT(capture.disp.committed_newlines == 2);
    capture_free(&capture);
}

static void test_printf_uses_display_newline_handling(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_printf(&capture.disp, "n=%d %s\n", 42, "rows");

    EXPECT_STR_EQ(capture_read(&capture), "n=42 rows");
    EXPECT(capture.disp.pending_newlines == 1);
    capture_free(&capture);
}

static void test_ansi_write_does_not_commit_pending_newline(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "body\n", 5);
    disp_write_ansi(&capture.disp, "\x1b[1m");

    EXPECT_STR_EQ(capture_read(&capture), "body\x1b[1m");
    EXPECT(capture.disp.pending_newlines == 1);
    capture_free(&capture);
}

static void test_external_line_resynchronizes_separator(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    capture.disp.pending_newlines = 3;
    fputs("external\n", capture.stream);
    disp_sync_external_line(&capture.disp);
    disp_block_separator(&capture.disp);

    EXPECT_STR_EQ(capture_read(&capture), "external\n\n");
    EXPECT(capture.disp.committed_newlines == 2);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_null_sink_selects_stdout(void)
{
    struct disp disp = {0};
    EXPECT(disp_sink(&disp) == stdout);
}

int main(void)
{
    test_putc_writes_visible_bytes();
    test_putc_defers_trailing_newlines();
    test_visible_byte_commits_pending_newlines();
    test_write_defers_trailing_newlines();
    test_write_normalizes_trailing_crlf();
    test_write_preserves_trailing_carriage_return();
    test_putc_carriage_return_preserves_newline_state();
    test_write_carriage_return_preserves_newline_state();
    test_empty_write_is_noop();
    test_block_separator_starts_with_blank_line();
    test_block_separator_collapses_pending_newlines();
    test_block_separator_preserves_existing_blank_line();
    test_printf_uses_display_newline_handling();
    test_ansi_write_does_not_commit_pending_newline();
    test_external_line_resynchronizes_separator();
    test_null_sink_selects_stdout();

    T_REPORT();
}
