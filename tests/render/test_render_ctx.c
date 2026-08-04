/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "render/disp.h"
#include "render/render_ctx.h"

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

static const char *fixture_read(struct fixture *fixture)
{
    disp_flush(&fixture->render.disp);
    return fixture->bytes ? fixture->bytes : "";
}

static void fixture_free(struct fixture *fixture)
{
    fclose(fixture->stream);
    free(fixture->bytes);
}

static void test_text_delta_ignores_initial_line_endings(void)
{
    struct fixture fixture;
    if (!fixture_init(&fixture))
        return;

    render_text_delta(&fixture.render, "\r\n\r", 3);
    EXPECT(fixture.render.state == RS_IDLE);
    EXPECT(!fixture.render.text_started);

    render_text_delta(&fixture.render, "\n\n\thello", 8);
    EXPECT_STR_EQ(fixture_read(&fixture), "\thello");
    EXPECT(fixture.render.state == RS_TEXT);
    EXPECT(fixture.render.text_started);
    fixture_free(&fixture);
}

static void test_text_delta_preserves_line_endings_after_text_starts(void)
{
    struct fixture fixture;
    if (!fixture_init(&fixture))
        return;

    render_text_delta(&fixture.render, "first", 5);
    render_text_delta(&fixture.render, "\nsecond", 7);

    EXPECT_STR_EQ(fixture_read(&fixture), "first\nsecond");
    fixture_free(&fixture);
}

int main(void)
{
    test_text_delta_ignores_initial_line_endings();
    test_text_delta_preserves_line_endings_after_text_starts();

    T_REPORT();
}
