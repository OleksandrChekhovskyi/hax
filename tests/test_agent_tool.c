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

static char *record_run(const char *args, tool_emit_display_fn emit, void *user)
{
    free(last_args);
    last_args = xstrdup(args);
    if (emit)
        emit("preview", 7, user);
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

    char *output = agent_tool_call_run(&tc, record_emit, NULL);
    EXPECT_STR_EQ(last_args, "{\"path\":\"rewritten\"}");
    EXPECT(emit_calls == 1);

    struct item result = agent_tool_result_make(&call, output);
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
    char *output = agent_tool_call_run(&tc, NULL, NULL);
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

    char *output = agent_tool_call_run(&tc, NULL, NULL);
    EXPECT_STR_EQ(last_args, "{\"command\":\"pwd\"}");

    free(output);
    agent_tool_call_destroy(&tc);
    item_free(&call);
}

int main(void)
{
    test_preprocess_run_and_result();
    test_unknown_tool();
    test_unmodified_args();
    free(last_args);
    T_REPORT();
}
