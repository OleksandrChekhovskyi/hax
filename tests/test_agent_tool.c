/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "agent_tool.h"
#include "harness.h"
#include "provider.h"
#include "tool.h"
#include "xalloc.h"

static char *last_args;
static int preprocess_calls;
static int display_calls;

static char *rewrite_args(const char *args)
{
    preprocess_calls++;
    EXPECT_STR_EQ(args, "{\"path\":\"original\"}");
    return xstrdup("{\"path\":\"rewritten\"}");
}

static char *record_run(const char *args, struct tool_run_ctx *ctx)
{
    free(last_args);
    last_args = xstrdup(args);
    if (ctx && ctx->display)
        ctx->display("preview", 7, ctx->display_data);
    return xstrdup("ok\a\n");
}

static void record_display(const char *bytes, size_t n, void *data)
{
    (void)data;
    EXPECT(n == 7);
    EXPECT(strncmp(bytes, "preview", n) == 0);
    display_calls++;
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
    display_calls = 0;

    agent_tool_call_init(&tc, &call);
    EXPECT(tc.tool == &TOOL_READ);
    EXPECT(preprocess_calls == 1);
    EXPECT_STR_EQ(call.tool_arguments_json, "{\"path\":\"original\"}");
    EXPECT_STR_EQ(tc.effective.tool_arguments_json, "{\"path\":\"rewritten\"}");

    struct tool_run_ctx ctx = {.display = record_display};
    char *output = agent_tool_call_run(&tc, &ctx);
    EXPECT_STR_EQ(last_args, "{\"path\":\"rewritten\"}");
    EXPECT(display_calls == 1);

    struct item result = agent_tool_result_make(&call, output, &ctx);
    EXPECT(result.kind == ITEM_TOOL_RESULT);
    EXPECT_STR_EQ(result.call_id, "call-1");
    EXPECT_STR_EQ(result.output, "ok\n");

    free(output);
    item_free(&result);
    agent_tool_call_destroy(&tc);
    item_free(&call);
}

static void test_result_make_interrupted_provenance(void)
{
    struct item call = make_call("read", "{}");
    struct tool_run_ctx ctx = {.interrupted = 1};

    struct item result = agent_tool_result_make(&call, "partial\n[interrupted]", &ctx);
    EXPECT(result.origin == ITEM_ORIGIN_INTERRUPTED);

    item_free(&result);
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
    EXPECT(tc.owned_args_json == NULL);
    EXPECT(tc.effective.tool_arguments_json == call.tool_arguments_json);

    char *output = agent_tool_call_run(&tc, NULL);
    EXPECT_STR_EQ(last_args, "{\"command\":\"pwd\"}");

    free(output);
    agent_tool_call_destroy(&tc);
    item_free(&call);
}

static struct item_image *make_image(size_t base64_len)
{
    struct item_image *image = xcalloc(1, sizeof(*image));
    image->mime = xstrdup("image/png");
    image->data_b64 = xmalloc(base64_len + 1);
    memset(image->data_b64, 'A', base64_len);
    image->data_b64[base64_len] = '\0';
    return image;
}

static struct item make_image_result(size_t base64_len)
{
    return (struct item){.kind = ITEM_TOOL_RESULT,
                         .call_id = xstrdup("c1"),
                         .output = xstrdup("Read image x.png"),
                         .images = make_image(base64_len),
                         .n_images = 1};
}

static void test_result_moves_run_context(void)
{
    struct item call = make_call("read", "{}");
    struct tool_run_ctx ctx = {
        .result_images = make_image(8),
        .n_result_images = 1,
        .output_summarizes_display = 1,
        .output_hidden_tail = 4,
    };

    struct item result = agent_tool_result_make(&call, "summary", &ctx);

    EXPECT(result.images != NULL);
    EXPECT(result.n_images == 1);
    EXPECT(result.origin == ITEM_ORIGIN_SUMMARIZED);
    EXPECT(result.output_hidden_tail == 4);
    EXPECT(ctx.result_images == NULL);
    EXPECT(ctx.n_result_images == 0);
    item_free(&result);
    item_free(&call);
}

static void test_image_budget_accepts_result(void)
{
    const size_t five_mb = 5u * 1024 * 1024;
    struct item history[] = {make_image_result(five_mb)};
    struct item result = make_image_result(five_mb);

    agent_tool_result_enforce_image_budget(history, 1, &result);

    EXPECT(result.n_images == 1);
    EXPECT(strstr(result.output, "not attached") == NULL);
    item_free(&result);
    item_free(&history[0]);
}

static void test_image_budget_drops_new_image(void)
{
    struct item history[] = {make_image_result(8u * 1024 * 1024),
                             make_image_result(8u * 1024 * 1024)};
    struct item result = make_image_result(5u * 1024 * 1024);

    agent_tool_result_enforce_image_budget(history, 2, &result);

    EXPECT(result.n_images == 0);
    EXPECT(result.images == NULL);
    EXPECT(strstr(result.output, "not attached") != NULL);
    EXPECT(strstr(result.output, "Read image x.png") != NULL);
    /* The note is a model-only tail: exactly the bytes appended after the original output. */
    EXPECT(result.output_hidden_tail == strlen(result.output) - strlen("Read image x.png"));
    EXPECT(history[0].n_images == 1 && history[1].n_images == 1);
    item_free(&result);
    item_free(&history[0]);
    item_free(&history[1]);
}

static void test_image_budget_enforces_count(void)
{
    struct item *history = xcalloc(IMAGE_REQUEST_MAX_COUNT, sizeof(*history));
    for (size_t i = 0; i < IMAGE_REQUEST_MAX_COUNT; i++)
        history[i] = make_image_result(64);
    struct item result = make_image_result(64);

    agent_tool_result_enforce_image_budget(history, IMAGE_REQUEST_MAX_COUNT, &result);

    EXPECT(result.n_images == 0);
    EXPECT(strstr(result.output, "too many images") != NULL);
    item_free(&result);
    for (size_t i = 0; i < IMAGE_REQUEST_MAX_COUNT; i++)
        item_free(&history[i]);
    free(history);
}

static void test_image_budget_ignores_plain_result(void)
{
    struct item result = {.kind = ITEM_TOOL_RESULT, .output = xstrdup("ok")};

    agent_tool_result_enforce_image_budget(NULL, 0, &result);

    EXPECT_STR_EQ(result.output, "ok");
    item_free(&result);
}

int main(void)
{
    test_preprocess_run_and_result();
    test_result_make_interrupted_provenance();
    test_unknown_tool();
    test_unmodified_args();
    test_result_moves_run_context();
    test_image_budget_accepts_result();
    test_image_budget_drops_new_image();
    test_image_budget_enforces_count();
    test_image_budget_ignores_plain_result();
    free(last_args);
    T_REPORT();
}
