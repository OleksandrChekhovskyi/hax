/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "render/disp.h"
#include "render/markdown.h"
#include "render/render_ctx.h"
#include "terminal/ansi.h"

struct fixture {
    struct render_ctx render;
    FILE *stream;
    char *bytes;
    size_t len;
};

static int fixture_init(struct fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->stream = open_memstream(&fixture->bytes, &fixture->len);
    EXPECT(fixture->stream != NULL);
    if (!fixture->stream)
        return 0;
    fixture->render.disp.sink = fixture->stream;
    fixture->render.disp.committed_newlines = 2;
    return 1;
}

static const char *fixture_output(struct fixture *fixture)
{
    disp_flush(&fixture->render.disp);
    return fixture->bytes ? fixture->bytes : "";
}

static void emit_markdown(const char *bytes, size_t len, int is_raw, void *user)
{
    struct disp *disp = user;
    if (is_raw)
        fwrite(bytes, 1, len, disp_sink(disp));
    else
        disp_write(disp, bytes, len);
}

static void fixture_enable_markdown(struct fixture *fixture)
{
    fixture->render.md = md_new(emit_markdown, &fixture->render.disp, 40);
}

static void fixture_destroy(struct fixture *fixture)
{
    md_free(fixture->render.md);
    fclose(fixture->stream);
    free(fixture->bytes);
}

static void test_text_delta_ignores_initial_line_endings(void)
{
    struct fixture fixture;
    if (!fixture_init(&fixture))
        return;

    const char *line_endings = "\r\n\r";
    render_text_delta(&fixture.render, line_endings, strlen(line_endings));
    EXPECT(fixture.render.mode == RENDER_IDLE);
    EXPECT(!fixture.render.stream.answer_started);

    const char *text = "\n\n\thello";
    render_text_delta(&fixture.render, text, strlen(text));
    EXPECT_STR_EQ(fixture_output(&fixture), "\thello");
    EXPECT(fixture.render.mode == RENDER_TEXT);
    EXPECT(fixture.render.stream.answer_started);
    fixture_destroy(&fixture);
}

static void test_text_delta_preserves_line_endings_after_text_starts(void)
{
    struct fixture fixture;
    if (!fixture_init(&fixture))
        return;

    render_text_delta(&fixture.render, "first", strlen("first"));
    render_text_delta(&fixture.render, "\nsecond", strlen("\nsecond"));

    EXPECT_STR_EQ(fixture_output(&fixture), "first\nsecond");
    fixture_destroy(&fixture);
}

static void test_closing_reasoning_resets_style(void)
{
    struct fixture fixture;
    if (!fixture_init(&fixture))
        return;

    render_set_mode(&fixture.render, RENDER_REASONING);
    render_write_text(&fixture.render, "thought", strlen("thought"));
    render_set_mode(&fixture.render, RENDER_IDLE);
    disp_commit_newlines(&fixture.render.disp);

    EXPECT_STR_EQ(fixture_output(&fixture), ANSI_DIM ANSI_ITALIC "thought" ANSI_RESET "\n");
    fixture_destroy(&fixture);
}

static void test_table_spinner_tracks_buffered_table(void)
{
    struct fixture fixture;
    if (!fixture_init(&fixture))
        return;
    fixture_enable_markdown(&fixture);

    const char *header = "| A |\n|---|\n";
    render_text_delta(&fixture.render, header, strlen(header));
    EXPECT(md_in_table(fixture.render.md));
    EXPECT(fixture.render.table.started_at_ms > 0);

    long started_at_ms = fixture.render.table.started_at_ms;
    render_show_table_spinner(&fixture.render);
    EXPECT(fixture.render.table.spinner_visible);

    const char *row = "| x |\n";
    render_write_text(&fixture.render, row, strlen(row));
    EXPECT(md_in_table(fixture.render.md));
    EXPECT(fixture.render.table.spinner_visible);
    EXPECT(fixture.render.table.started_at_ms == started_at_ms);

    const char *after = "after\n";
    render_write_text(&fixture.render, after, strlen(after));
    EXPECT(!md_in_table(fixture.render.md));
    EXPECT(!fixture.render.table.spinner_visible);
    EXPECT(fixture.render.table.started_at_ms == 0);
    fixture_destroy(&fixture);
}

static void test_stream_begin_resets_per_stream_state(void)
{
    struct fixture fixture;
    if (!fixture_init(&fixture))
        return;

    fixture.render.table.started_at_ms = 10;
    fixture.render.retry.deadline_ms = 20;
    fixture.render.retry.next_attempt = 2;
    fixture.render.retry.max_attempts = 3;
    fixture.render.stream.content_seen = 1;
    fixture.render.stream.answer_started = 1;
    render_stream_begin(&fixture.render);

    EXPECT(fixture.render.mode == RENDER_WAITING);
    EXPECT(fixture.render.table.started_at_ms == 0);
    EXPECT(fixture.render.retry.deadline_ms == 0);
    EXPECT(fixture.render.retry.next_attempt == 0);
    EXPECT(fixture.render.retry.max_attempts == 0);
    EXPECT(!fixture.render.stream.content_seen);
    EXPECT(!fixture.render.stream.answer_started);
    fixture_destroy(&fixture);
}

static void test_stream_begin_preserves_tool_cluster(void)
{
    struct fixture fixture;
    if (!fixture_init(&fixture))
        return;

    render_set_mode(&fixture.render, RENDER_TOOL_CLUSTER);
    fixture.render.cluster.last_tool = "read";
    fixture.render.cluster.line_open = 1;
    fixture.render.cluster.line_cells = 12;
    fixture.render.stream.content_seen = 1;
    render_stream_begin(&fixture.render);

    EXPECT(fixture.render.mode == RENDER_TOOL_CLUSTER);
    EXPECT_STR_EQ(fixture.render.cluster.last_tool, "read");
    EXPECT(fixture.render.cluster.line_open);
    EXPECT(fixture.render.cluster.line_cells == 12);
    EXPECT(!fixture.render.stream.content_seen);
    fixture_destroy(&fixture);
}

static void test_closing_tool_cluster_resets_substate(void)
{
    struct fixture fixture;
    if (!fixture_init(&fixture))
        return;

    render_set_mode(&fixture.render, RENDER_TOOL_CLUSTER);
    fixture.render.cluster.last_tool = "read";
    fixture.render.cluster.line_open = 1;
    fixture.render.cluster.line_cells = 12;
    render_set_mode(&fixture.render, RENDER_IDLE);

    EXPECT(fixture.render.cluster.last_tool == NULL);
    EXPECT(!fixture.render.cluster.line_open);
    EXPECT(fixture.render.cluster.line_cells == 0);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_text_delta_ignores_initial_line_endings();
    test_text_delta_preserves_line_endings_after_text_starts();
    test_closing_reasoning_resets_style();
    test_table_spinner_tracks_buffered_table();
    test_stream_begin_resets_per_stream_state();
    test_stream_begin_preserves_tool_cluster();
    test_closing_tool_cluster_resets_substate();

    T_REPORT();
}
