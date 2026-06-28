/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harness.h"
#include "util.h"
#include "tool.h"
#include "provider.h"
#include "agent_dispatch.h"
#include "render/markdown.h"
#include "render/render_ctx.h"
#include "render/spinner.h"

/* find_tool lives in agent_core.c alongside the full tool table; stub it
 * so this test links only `write` (the tool under test) and the render
 * stack rather than every tool and its dependencies. */
const struct tool *find_tool(const char *name)
{
    return strcmp(name, "write") == 0 ? &TOOL_WRITE : NULL;
}

/* render_ctx references the markdown sink, but every md_* call is guarded
 * by `r->md != NULL` and we run with md == NULL — stub these so we don't
 * have to link markdown.c. None are ever invoked. */
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

/* Redirect stdout to a regular file so isatty() is false and the spinner stays
 * synchronous, matching test_tool_render. */

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
    struct render_ctx r = {0};
    r.state = RS_IDLE;
    r.spinner = spinner_new(NULL);
    *out = dispatch_tool_call(&r, &call);
    const char *cap = cap_read();
    spinner_free(r.spinner);

    free(call.call_id);
    free(call.tool_name);
    free(call.tool_arguments_json);
    free(args);
    return cap;
}

static char *mk_tmpdir(void)
{
    char *dir = xstrdup("/tmp/hax-dispatch-XXXXXX");
    if (!mkdtemp(dir)) {
        FAIL("mkdtemp: %s", strerror(errno));
        free(dir);
        return NULL;
    }
    return dir;
}

static void rm_rf(const char *dir)
{
    char *cmd = xasprintf("rm -rf '%s'", dir);
    (void)system(cmd);
    free(cmd);
}

/* The regression this guards: a new-file write whose streamed content
 * preview renders zero rows (blank or control-only content) must fall
 * back to showing the "created ..." summary as the block body, not a bare
 * [write] header. Asserting only the return string (as the write unit test
 * does) would not catch removal of that dispatch-level fallback, since the
 * fallback is what puts the summary on screen. */
static void test_blank_content_summary_row_displayed(void)
{
    char *dir = mk_tmpdir();
    char *path = xasprintf("%s/blank.py", dir);
    struct item out;

    const char *cap = run_write(path, "\\n   \\n", &out); /* only blank lines */
    EXPECT(strstr(cap, "created") != NULL);
    EXPECT(strstr(cap, "blank.py") != NULL);
    /* The model still receives the summary regardless of display. */
    EXPECT(out.output && strstr(out.output, "created") != NULL);

    item_free(&out);
    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_control_only_content_summary_row_displayed(void)
{
    /* The case a byte-class predicate would miss: ctrl_strip removes a
     * lone bell (and would swallow an ANSI escape with its trailing
     * bytes), leaving zero rows. The fallback keys off the actual row
     * count, so it still surfaces the summary. */
    char *dir = mk_tmpdir();
    char *path = xasprintf("%s/bell.py", dir);
    struct item out;

    const char *cap = run_write(path, "\\u0007", &out); /* single bell byte */
    EXPECT(strstr(cap, "created") != NULL);
    EXPECT(strstr(cap, "bell.py") != NULL);

    item_free(&out);
    rm_rf(dir);
    free(path);
    free(dir);
}

static void test_visible_content_shows_preview_not_summary(void)
{
    /* Contrast: content that renders rows shows the preview, and the
     * fallback must NOT fire — the "created ..." summary goes only to the
     * model, not the display. */
    char *dir = mk_tmpdir();
    char *path = xasprintf("%s/real.py", dir);
    struct item out;

    const char *cap = run_write(path, "hello world", &out);
    EXPECT(strstr(cap, "hello world") != NULL);
    EXPECT(strstr(cap, "created") == NULL);
    /* Model side still gets the terse summary, not the echoed content. */
    EXPECT(out.output && strstr(out.output, "created") != NULL);

    item_free(&out);
    rm_rf(dir);
    free(path);
    free(dir);
}

int main(void)
{
    cap_init();
    test_blank_content_summary_row_displayed();
    test_control_only_content_summary_row_displayed();
    test_visible_content_shows_preview_not_summary();
    T_REPORT();
}
