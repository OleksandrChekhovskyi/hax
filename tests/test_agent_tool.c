/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "agent_tool.h"
#include "harness.h"
#include "util.h"

static char *last_args;
static int preprocess_calls;
static int emit_calls;

static char *rewrite_args(const char *args)
{
    preprocess_calls++;
    EXPECT_STR_EQ(args, "{\"path\":\"original\"}");
    return xstrdup("{\"path\":\"rewritten\"}");
}

static char *record_run(const char *args, struct tool_ctx *ctx)
{
    free(last_args);
    last_args = xstrdup(args);
    if (ctx && ctx->emit_display)
        ctx->emit_display("preview", 7, ctx->emit_user);
    return xstrdup("ok\a\n");
}

static int record_emit(const char *bytes, size_t n, void *user)
{
    (void)user;
    EXPECT(n == 7);
    EXPECT(strncmp(bytes, "preview", n) == 0);
    emit_calls++;
    return 0;
}

const struct tool TOOL_READ = {
    .def = {.name = "read"},
    .run = record_run,
    .preprocess_args = rewrite_args,
};
const struct tool TOOL_BASH = {.def = {.name = "bash"}, .run = record_run};
const struct tool TOOL_WRITE = {.def = {.name = "write"}, .run = record_run};
const struct tool TOOL_EDIT = {.def = {.name = "edit"}, .run = record_run};

static struct item make_call(const char *name, const char *args)
{
    return (struct item){
        .kind = ITEM_TOOL_CALL,
        .call_id = xstrdup("call-1"),
        .tool_name = xstrdup(name),
        .tool_arguments_json = xstrdup(args),
    };
}

static void test_preprocess_run_and_result(void)
{
    struct item call = make_call("read", "{\"path\":\"original\"}");
    struct agent_tool_call tc;
    preprocess_calls = 0;
    emit_calls = 0;

    agent_tool_call_init(&tc, &call);
    EXPECT(tc.tool == &TOOL_READ);
    EXPECT(preprocess_calls == 1);
    EXPECT_STR_EQ(call.tool_arguments_json, "{\"path\":\"original\"}");
    EXPECT_STR_EQ(tc.effective.tool_arguments_json, "{\"path\":\"rewritten\"}");

    struct tool_ctx ctx = {.emit_display = record_emit};
    char *output = agent_tool_call_run(&tc, &ctx);
    EXPECT_STR_EQ(last_args, "{\"path\":\"rewritten\"}");
    EXPECT(emit_calls == 1);

    struct item result = agent_tool_result_make(&call, output, &ctx);
    EXPECT(result.kind == ITEM_TOOL_RESULT);
    EXPECT_STR_EQ(result.call_id, "call-1");
    EXPECT_STR_EQ(result.output, "ok\n");

    free(output);
    item_free(&result);
    agent_tool_call_destroy(&tc);
    item_free(&call);
}

static void test_unknown_tool(void)
{
    struct item call = make_call("missing", "{}");
    struct agent_tool_call tc;
    agent_tool_call_init(&tc, &call);

    EXPECT(tc.tool == NULL);
    char *output = agent_tool_call_run(&tc, NULL);
    EXPECT_STR_EQ(output, "unknown tool: missing");

    free(output);
    agent_tool_call_destroy(&tc);
    item_free(&call);
}

static void test_unmodified_args(void)
{
    struct item call = make_call("bash", "{\"command\":\"pwd\"}");
    struct agent_tool_call tc;
    agent_tool_call_init(&tc, &call);

    EXPECT(tc.tool == &TOOL_BASH);
    EXPECT(tc.rewritten_args == NULL);
    EXPECT(tc.effective.tool_arguments_json == call.tool_arguments_json);

    char *output = agent_tool_call_run(&tc, NULL);
    EXPECT_STR_EQ(last_args, "{\"command\":\"pwd\"}");

    free(output);
    agent_tool_call_destroy(&tc);
    item_free(&call);
}

/* A tool_result owning one image with `b64_len` bytes of filler base64. */
static struct item make_image_result(size_t b64_len)
{
    struct item_image *img = xcalloc(1, sizeof(*img));
    img->mime = xstrdup("image/png");
    img->data_b64 = xmalloc(b64_len + 1);
    memset(img->data_b64, 'A', b64_len);
    img->data_b64[b64_len] = '\0';
    return (struct item){.kind = ITEM_TOOL_RESULT,
                         .call_id = xstrdup("c1"),
                         .output = xstrdup("Read image x.png"),
                         .images = img,
                         .n_images = 1};
}

static void test_image_budget_enforce(void)
{
    const size_t five_mb = 5u * 1024 * 1024;

    /* History well under budget: the new image is kept untouched. */
    struct item hist_ok[] = {make_image_result(five_mb)};
    struct item result = make_image_result(five_mb);
    image_budget_enforce(hist_ok, 1, &result);
    EXPECT(result.n_images == 1);
    EXPECT(strstr(result.output, "not attached") == NULL);
    item_free(&result);
    item_free(&hist_ok[0]);

    /* History near the byte budget: adding another 5 MiB tips over, so the
     * new image is dropped and a recoverable note appended — the history
     * items are never touched (cache-safe). */
    struct item hist_full[] = {make_image_result(8u * 1024 * 1024),
                               make_image_result(8u * 1024 * 1024)};
    result = make_image_result(five_mb);
    image_budget_enforce(hist_full, 2, &result);
    EXPECT(result.n_images == 0);
    EXPECT(result.images == NULL);
    EXPECT(strstr(result.output, "not attached") != NULL);
    EXPECT(strstr(result.output, "Read image x.png") != NULL); /* original text kept */
    EXPECT(hist_full[0].n_images == 1 && hist_full[1].n_images == 1);
    item_free(&result);
    item_free(&hist_full[0]);
    item_free(&hist_full[1]);

    /* Count cap: many tiny images stay under the byte budget but a history
     * already holding IMAGE_REQUEST_MAX_COUNT of them refuses the next one,
     * with a count-specific note. */
    struct item *tiny = xcalloc(IMAGE_REQUEST_MAX_COUNT, sizeof(*tiny));
    for (size_t i = 0; i < IMAGE_REQUEST_MAX_COUNT; i++)
        tiny[i] = make_image_result(64);
    result = make_image_result(64);
    image_budget_enforce(tiny, IMAGE_REQUEST_MAX_COUNT, &result);
    EXPECT(result.n_images == 0);
    EXPECT(strstr(result.output, "too many images") != NULL);
    item_free(&result);
    for (size_t i = 0; i < IMAGE_REQUEST_MAX_COUNT; i++)
        item_free(&tiny[i]);
    free(tiny);

    /* A result with no images is a no-op regardless of history. */
    struct item plain = {.kind = ITEM_TOOL_RESULT, .output = xstrdup("ok")};
    image_budget_enforce(hist_full, 0, &plain);
    EXPECT_STR_EQ(plain.output, "ok");
    item_free(&plain);
}

int main(void)
{
    test_preprocess_run_and_result();
    test_unknown_tool();
    test_unmodified_args();
    test_image_budget_enforce();
    free(last_args);
    T_REPORT();
}
