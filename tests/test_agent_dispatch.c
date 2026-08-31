/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_dispatch.h"
#include "harness.h"
#include "provider.h"
#include "tool.h"
#include "xalloc.h"
#include "render/markdown.h"
#include "render/render_ctx.h"
#include "render/spinner.h"
#include "system/locale.h"

static char *run_mode_probe(const char *args_json, struct tool_run_ctx *ctx)
{
    (void)args_json;
    (void)ctx;
    return xstrdup("row1\nrow2\nrow3\nrow4\nrow5\nrow6\nrow7\nrow8\nrow9\ntail-marker\n");
}

static enum tool_preview_mode select_head(const char *args_json)
{
    (void)args_json;
    return TOOL_PREVIEW_HEAD;
}

static const struct tool TOOL_MODE_PROBE = {
    .def = {.name = "mode-probe"},
    .run = run_mode_probe,
    .display = {.preview_mode = TOOL_PREVIEW_HEAD_TAIL, .select_preview = select_head},
};

/* A tool that returns a hidden tail without ever streaming display bytes. */
static char *run_tail_probe(const char *args_json, struct tool_run_ctx *ctx)
{
    (void)args_json;
    ctx->output_hidden_tail = strlen("\n[model-only note]");
    return xstrdup("body-line\n[model-only note]");
}

static const struct tool TOOL_TAIL_PROBE = {
    .def = {.name = "tail-probe"},
    .run = run_tail_probe,
    .display = {.preview_mode = TOOL_PREVIEW_HEAD_TAIL},
};

/* agent_find_tool lives in agent_core.c alongside the full tool table; stub it
 * so this test links only its own tools and the render stack. */
const struct tool *agent_find_tool(const char *name)
{
    if (strcmp(name, "write") == 0)
        return &TOOL_WRITE;
    if (strcmp(name, "mode-probe") == 0)
        return &TOOL_MODE_PROBE;
    if (strcmp(name, "tail-probe") == 0)
        return &TOOL_TAIL_PROBE;
    return NULL;
}

/* These stubs are not called because the fixtures leave render.md NULL. */
int md_cols(void)
{
    return 0;
}

void md_reset(struct md_renderer *m, int wrap_width)
{
    (void)m;
    (void)wrap_width;
}

void md_feed(struct md_renderer *m, const char *s, size_t n)
{
    (void)m;
    (void)s;
    (void)n;
}

void md_flush(struct md_renderer *m)
{
    (void)m;
}

void md_set_styled(struct md_renderer *m, int on)
{
    (void)m;
    (void)on;
}

int md_in_table(const struct md_renderer *m)
{
    (void)m;
    return 0;
}

/* ---- stdout capture (freopen → regular file, so isatty()→0 and the
 * spinner stays synchronous; same approach as test_tool_render). ---- */

static char captured[131072];

static void cap_init(void)
{
    locale_init_utf8();
    char path[64];
    snprintf(path, sizeof(path), "/tmp/haxdispatch.%d.out", (int)getpid());
    if (!freopen(path, "w+", stdout)) {
        perror("freopen");
        exit(1);
    }
    unlink(path);
}

static void cap_reset(void)
{
    fflush(stdout);
    if (ftruncate(fileno(stdout), 0) != 0) {
        perror("ftruncate");
        exit(1);
    }
    rewind(stdout);
}

static const char *cap_read(void)
{
    fflush(stdout);
    fseek(stdout, 0, SEEK_SET);
    size_t n = fread(captured, 1, sizeof(captured) - 1, stdout);
    captured[n] = 0;
    return captured;
}

/* Run a `write` call through the verbose dispatch path and return the
 * captured display bytes. `out` receives the tool_result (model-facing
 * text); caller frees it. `content_json` is the raw JSON string value for
 * the file content (e.g. "\\n" for a newline, "\\u0007" for a bell). */
static const char *run_write(const char *path, const char *content_json, struct item *out)
{
    char *args = xasprintf("{\"path\":\"%s\",\"content\":\"%s\"}", path, content_json);
    struct item call = {0};
    call.kind = ITEM_TOOL_CALL;
    call.call_id = xstrdup("call-1");
    call.tool_name = xstrdup("write");
    call.tool_arguments_json = xstrdup(args);

    cap_reset();
    struct render_ctx render = {0};
    render.mode = RENDER_IDLE;
    render.spinner = spinner_new(NULL);
    *out = dispatch_tool_call(&render, &call, -1);
    const char *cap = cap_read();
    spinner_free(render.spinner);

    free(call.call_id);
    free(call.tool_name);
    free(call.tool_arguments_json);
    free(args);
    return cap;
}

static void test_blank_content_summary_row_displayed(void)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/blank.py", dir);
    struct item out;

    const char *cap = run_write(path, "\\n   \\n", &out);
    EXPECT(strstr(cap, "created") != NULL);
    EXPECT(strstr(cap, "blank.py") != NULL);
    EXPECT(out.output && strstr(out.output, "created") != NULL);

    item_free(&out);
    free(path);
}

static void test_control_only_content_summary_row_displayed(void)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/bell.py", dir);
    struct item out;

    const char *cap = run_write(path, "\\u0007", &out);
    EXPECT(strstr(cap, "created") != NULL);
    EXPECT(strstr(cap, "bell.py") != NULL);

    item_free(&out);
    free(path);
}

static void test_visible_content_shows_preview_not_summary(void)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/real.py", dir);
    struct item out;

    const char *cap = run_write(path, "hello world", &out);
    EXPECT(strstr(cap, "hello world") != NULL);
    EXPECT(strstr(cap, "created") == NULL);
    EXPECT(out.output && strstr(out.output, "created") != NULL);

    item_free(&out);
    free(path);
}

static void test_selector_overrides_non_collapsed_mode(void)
{
    struct item call = {
        .kind = ITEM_TOOL_CALL,
        .call_id = xstrdup("call-mode"),
        .tool_name = xstrdup("mode-probe"),
        .tool_arguments_json = xstrdup("{}"),
    };
    struct render_ctx render = {.mode = RENDER_IDLE, .spinner = spinner_new(NULL)};

    cap_reset();
    struct item result = dispatch_tool_call(&render, &call, -1);
    const char *cap = cap_read();

    EXPECT(strstr(cap, "more line") != NULL);
    EXPECT(strstr(cap, "tail-marker") == NULL);
    item_free(&result);
    spinner_free(render.spinner);
    item_free(&call);
}

/* The verbose fallback renders the returned output when the tool never
 * streamed; it must honor the hidden-tail contract locally rather than
 * rely on every tail-producing tool also having called display. */
static void test_hidden_tail_not_displayed_by_fallback(void)
{
    struct item call = {
        .kind = ITEM_TOOL_CALL,
        .call_id = xstrdup("call-tail"),
        .tool_name = xstrdup("tail-probe"),
        .tool_arguments_json = xstrdup("{}"),
    };
    struct render_ctx render = {.mode = RENDER_IDLE, .spinner = spinner_new(NULL)};

    cap_reset();
    struct item result = dispatch_tool_call(&render, &call, -1);
    const char *cap = cap_read();

    EXPECT(strstr(cap, "body-line") != NULL);
    EXPECT(strstr(cap, "model-only note") == NULL);
    /* The model-facing result keeps the tail and its length. */
    EXPECT(result.output && strstr(result.output, "model-only note") != NULL);
    EXPECT(result.output_hidden_tail == strlen("\n[model-only note]"));
    item_free(&result);
    spinner_free(render.spinner);
    item_free(&call);
}

int main(void)
{
    cap_init();
    test_blank_content_summary_row_displayed();
    test_control_only_content_summary_row_displayed();
    test_visible_content_shows_preview_not_summary();
    test_selector_overrides_non_collapsed_mode();
    test_hidden_tail_not_displayed_by_fallback();
    T_REPORT();
}
