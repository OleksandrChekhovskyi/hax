/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "harness.h"
#include "tool.h"
#include "turn.h"
#include "util.h"

/* agent_core's static tool table requires these link-time stand-ins. */
static char *stub_run(const char *args, struct tool_run_ctx *ctx)
{
    (void)args;
    (void)ctx;
    return xstrdup("");
}

const struct tool TOOL_READ = {.def = {.name = "read"}, .run = stub_run};
const struct tool TOOL_EDIT = {.def = {.name = "edit"}, .run = stub_run};
const struct tool TOOL_WRITE = {.def = {.name = "write"}, .run = stub_run};
const struct tool TOOL_BASH = {.def = {.name = "bash"}, .run = stub_run};

static void test_session_append(void)
{
    struct agent_session session = {0};

    agent_session_append(&session,
                         (struct item){.kind = ITEM_USER_MESSAGE, .text = xstrdup("hello")});
    agent_session_append(&session, (struct item){.kind = ITEM_TURN_BOUNDARY});
    agent_session_append(&session,
                         (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup("world")});

    EXPECT(session.n_items == 3);
    EXPECT(session.items[0].kind == ITEM_USER_MESSAGE);
    EXPECT_STR_EQ(session.items[0].text, "hello");
    EXPECT(session.items[1].kind == ITEM_TURN_BOUNDARY);
    EXPECT(session.items[2].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT_STR_EQ(session.items[2].text, "world");

    agent_session_free(&session);
}

static void test_find_tool(void)
{
    EXPECT(agent_find_tool("read") == &TOOL_READ);
    EXPECT(agent_find_tool("bash") == &TOOL_BASH);
    EXPECT(agent_find_tool("write") == &TOOL_WRITE);
    EXPECT(agent_find_tool("edit") == &TOOL_EDIT);
    EXPECT(agent_find_tool("nonexistent") == NULL);
    EXPECT(agent_find_tool("") == NULL);
}

static char *build_test_system_prompt(int raw)
{
    setenv("HAX_MODEL", "model-x", 1);
    struct provider provider = {.name = "test"};
    struct hax_opts opts = {.raw = raw};
    struct agent_session session;
    agent_session_init(&session, &provider, &opts);
    char *prompt = session.system_prompt;
    session.system_prompt = NULL;
    agent_session_free(&session);
    unsetenv("HAX_MODEL");
    return prompt;
}

static void test_build_system_prompt_raw(void)
{
    setenv("HAX_SYSTEM_PROMPT", "ignored", 1);
    char *out = build_test_system_prompt(1);
    EXPECT(out == NULL);
    unsetenv("HAX_SYSTEM_PROMPT");
}

static void test_build_system_prompt_explicit_empty(void)
{
    setenv("HAX_SYSTEM_PROMPT", "", 1);
    char *out = build_test_system_prompt(0);
    EXPECT(out == NULL);
    unsetenv("HAX_SYSTEM_PROMPT");
}

static void test_build_system_prompt_custom_no_suffix(void)
{
    setenv("HAX_SYSTEM_PROMPT", "you are a teapot", 1);
    setenv("HAX_NO_ENV", "1", 1);
    setenv("HAX_NO_AGENTS_MD", "1", 1);
    setenv("HAX_NO_SKILLS", "1", 1);
    setenv("HAX_NO_SUBAGENTS", "1", 1);
    setenv("HAX_NO_TASKS", "1", 1);

    char *out = build_test_system_prompt(0);
    EXPECT(out != NULL);
    if (out)
        EXPECT_STR_EQ(out, "you are a teapot");
    free(out);

    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
    unsetenv("HAX_NO_SKILLS");
    unsetenv("HAX_NO_SUBAGENTS");
    unsetenv("HAX_NO_TASKS");
}

static void test_build_system_prompt_default_no_suffix(void)
{
    unsetenv("HAX_SYSTEM_PROMPT");
    setenv("HAX_NO_ENV", "1", 1);
    setenv("HAX_NO_AGENTS_MD", "1", 1);

    char *out = build_test_system_prompt(0);
    EXPECT(out != NULL);
    if (out)
        EXPECT(strncmp(out, "You are hax", 11) == 0);
    free(out);

    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
}

static void test_build_system_prompt_with_suffix(void)
{
    setenv("HAX_SYSTEM_PROMPT", "PREFIX", 1);
    setenv("HAX_NO_AGENTS_MD", "1", 1);
    unsetenv("HAX_NO_ENV");

    char *out = build_test_system_prompt(0);
    EXPECT(out != NULL);
    if (out) {
        EXPECT(strncmp(out, "PREFIX\n\n", 8) == 0);
        EXPECT(strstr(out, "# Environment") != NULL);
        EXPECT(strstr(out, "- Model: model-x") != NULL);
    }
    free(out);

    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_AGENTS_MD");
}

static const char *const test_effort_levels[] = {"low", "high"};

static size_t test_list_efforts(struct provider *p, const char *const **out)
{
    (void)p;
    *out = test_effort_levels;
    return 2;
}

/* `no_session = auto` splits on provider_factory.internal, so these assert
 * against the real registry: "mock" is the internal backend, "anthropic" a
 * user-facing one. HAX_PROVIDER is what agent_provider_id reads first, so it
 * — not p->name — decides when both are set. */
static void test_recording_enabled(void)
{
    struct provider mock = {.name = "mock"};
    struct provider real = {.name = "anthropic"};

    unsetenv("HAX_NO_SESSION");
    unsetenv("HAX_PROVIDER");

    /* auto (unset): real providers record, the dev backend doesn't. */
    EXPECT(agent_recording_enabled(&real) == 1);
    EXPECT(agent_recording_enabled(&mock) == 0);
    /* spelled out, same answers */
    setenv("HAX_NO_SESSION", "auto", 1);
    EXPECT(agent_recording_enabled(&real) == 1);
    EXPECT(agent_recording_enabled(&mock) == 0);

    /* explicit off wins for both — the escape hatch that lets a mock run
     * exercise the session and prompt-history paths it normally skips. */
    setenv("HAX_NO_SESSION", "0", 1);
    EXPECT(agent_recording_enabled(&mock) == 1);
    EXPECT(agent_recording_enabled(&real) == 1);

    /* explicit on wins for both, dev backend or not */
    setenv("HAX_NO_SESSION", "1", 1);
    EXPECT(agent_recording_enabled(&mock) == 0);
    EXPECT(agent_recording_enabled(&real) == 0);

    /* an unparseable value falls back to the auto rule rather than to a
     * fixed answer, so a typo can't silently start recording a mock run. */
    setenv("HAX_NO_SESSION", "banana", 1);
    EXPECT(agent_recording_enabled(&real) == 1);
    EXPECT(agent_recording_enabled(&mock) == 0);
    unsetenv("HAX_NO_SESSION");

    /* the configured id outranks p->name (it's what a resume feeds back to
     * provider_find), and an unknown one is treated as user-facing. */
    setenv("HAX_PROVIDER", "mock", 1);
    EXPECT(agent_recording_enabled(&real) == 0);
    setenv("HAX_PROVIDER", "not-a-provider", 1);
    EXPECT(agent_recording_enabled(&mock) == 1);
    unsetenv("HAX_PROVIDER");

    /* no provider resolved yet (startup before a /provider pick) records */
    EXPECT(agent_recording_enabled(NULL) == 1);
}

static void expect_effort(struct provider *provider, const char *model, const char *expected)
{
    setenv("HAX_MODEL", model, 1);
    struct hax_opts opts = {0};
    struct agent_session session;
    agent_session_init(&session, provider, &opts);
    if (expected)
        EXPECT_STR_EQ(session.effort, expected);
    else
        EXPECT(session.effort == NULL);
    agent_session_free(&session);
    unsetenv("HAX_MODEL");
}

static void test_resolve_effort(void)
{
    struct provider p = {
        .name = "test", .default_effort = "high", .list_efforts = test_list_efforts};

    /* unset → provider default */
    unsetenv("HAX_EFFORT");
    expect_effort(&p, "m", "high");

    /* explicit empty → "force omit" (NULL), even though provider has a default */
    setenv("HAX_EFFORT", "", 1);
    expect_effort(&p, "m", NULL);

    /* non-empty → passes through verbatim */
    setenv("HAX_EFFORT", "low", 1);
    expect_effort(&p, "m", "low");

    /* with no provider default and unset env, returns NULL */
    unsetenv("HAX_EFFORT");
    struct provider p2 = {
        .name = "test", .default_effort = NULL, .list_efforts = test_list_efforts};
    expect_effort(&p2, "m", NULL);

    /* a provider with no effort ladder (NULL hook, or one that reports zero
     * levels) never resolves an effort — even one persisted in config — so a
     * stale value can't leak onto e.g. llama.cpp / ollama. */
    setenv("HAX_EFFORT", "high", 1);
    struct provider p3 = {.name = "test", .default_effort = "high", .list_efforts = NULL};
    expect_effort(&p3, "m", NULL);

    /* A stale pick carried over from another backend lands on the nearest
     * level offered rather than being sent verbatim. test_list_efforts
     * offers {low, high}, so "medium" rounds down and keeps the user's
     * intent instead of reverting to the provider default. */
    setenv("HAX_EFFORT", "medium", 1);
    expect_effort(&p, "m", "low");
    expect_effort(&p2, "m", "low");
    /* Above everything offered clamps down too. */
    setenv("HAX_EFFORT", "xhigh", 1);
    expect_effort(&p, "m", "high");
    /* A name with no place in the ladder can't be clamped, so the provider
     * default answers — and when there is none, nothing is sent. */
    setenv("HAX_EFFORT", "ludicrous", 1);
    expect_effort(&p, "m", "high");
    expect_effort(&p2, "m", NULL);
    unsetenv("HAX_EFFORT");
}

static char *test_model_label(struct provider *p, const char *model)
{
    (void)p;
    (void)model;
    return xstrdup("short-model");
}

static void test_session_init_model_label(void)
{
    setenv("HAX_MODEL", "/models/long-model.gguf", 1);
    setenv("HAX_SYSTEM_PROMPT", "PREFIX", 1);
    setenv("HAX_NO_AGENTS_MD", "1", 1);
    unsetenv("HAX_NO_ENV");

    struct provider p = {.name = "test", .model_label = test_model_label};
    struct hax_opts opts = {0};
    struct agent_session s;
    agent_session_init(&s, &p, &opts);
    EXPECT_STR_EQ(s.model, "/models/long-model.gguf");
    EXPECT_STR_EQ(s.model_label, "short-model");
    EXPECT(s.system_prompt != NULL && strstr(s.system_prompt, "- Model: short-model") != NULL);
    EXPECT(s.system_prompt != NULL && strstr(s.system_prompt, "/models/long-model.gguf") == NULL);

    agent_session_free(&s);
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_AGENTS_MD");
}

static void test_session_init_raw(void)
{
    setenv("HAX_MODEL", "m-raw", 1);
    setenv("HAX_SYSTEM_PROMPT", "ignored", 1);

    struct provider p = {.name = "test", .default_model = NULL};
    struct hax_opts opts = {.raw = 1};

    struct agent_session s;
    agent_session_init(&s, &p, &opts);
    EXPECT(s.system_prompt == NULL);
    EXPECT(s.tools == NULL);
    EXPECT(s.n_tools == 0);
    EXPECT_STR_EQ(s.model, "m-raw");

    struct context ctx = agent_session_context(&s);
    EXPECT(ctx.system_prompt == NULL);
    EXPECT(ctx.tools == NULL);
    EXPECT(ctx.n_tools == 0);

    agent_session_free(&s);
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");
}

static void test_session_init_missing_model(void)
{
    unsetenv("HAX_MODEL");
    struct provider p = {.name = "test"};
    struct hax_opts opts = {0};

    struct agent_session s;
    agent_session_init(&s, &p, &opts);
    EXPECT(s.model == NULL);
    agent_session_free(&s);
}

static void test_session_init_missing_provider(void)
{
    unsetenv("HAX_MODEL");
    unsetenv("HAX_EFFORT");
    setenv("HAX_SYSTEM_PROMPT", "", 1);
    struct hax_opts opts = {0};

    struct agent_session s;
    agent_session_init(&s, NULL, &opts);
    EXPECT(s.model == NULL);
    EXPECT(s.provider_name == NULL);
    EXPECT(s.effort == NULL);
    agent_session_free(&s);

    unsetenv("HAX_SYSTEM_PROMPT");
}

static void test_session_add_user(void)
{
    struct agent_session s = {0};
    agent_session_add_user(&s, "hi there");

    EXPECT(s.n_items == 2);
    EXPECT(s.items[0].kind == ITEM_TURN_BOUNDARY);
    EXPECT(s.items[1].kind == ITEM_USER_MESSAGE);
    EXPECT_STR_EQ(s.items[1].text, "hi there");

    agent_session_add_boundary(&s);
    EXPECT(s.n_items == 3);
    EXPECT(s.items[2].kind == ITEM_TURN_BOUNDARY);

    agent_session_free(&s);
    EXPECT(s.items == NULL);
    EXPECT(s.n_items == 0);
}

static void feed_turn(struct turn *turn, struct stream_event event)
{
    turn_consume(turn, &event);
}

static void test_session_absorb_no_tool_call(void)
{
    struct agent_session s = {0};
    agent_session_add_user(&s, "go");

    struct turn t;
    turn_init(&t);
    feed_turn(&t, (struct stream_event){.kind = EV_TEXT_DELTA, .u.text_delta = {.text = "answer"}});
    feed_turn(&t, (struct stream_event){.kind = EV_DONE});

    struct agent_absorb_result absorbed = agent_session_absorb(&s, &t);
    turn_reset(&t);

    EXPECT(absorbed.items_from == 2);
    EXPECT(!absorbed.had_tool_call);
    EXPECT(s.n_items == 3);
    EXPECT(s.items[2].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT_STR_EQ(s.items[2].text, "answer");

    agent_session_free(&s);
}

static void test_session_absorb_with_tool_call(void)
{
    struct agent_session s = {0};

    struct turn t;
    turn_init(&t);
    feed_turn(&t, (struct stream_event){.kind = EV_TOOL_CALL_START,
                                        .u.tool_call_start = {.id = "c1", .name = "bash"}});
    feed_turn(&t, (struct stream_event){.kind = EV_TOOL_CALL_DELTA,
                                        .u.tool_call_delta = {.id = "c1", .args_delta = "{}"}});
    feed_turn(&t, (struct stream_event){.kind = EV_TOOL_CALL_END, .u.tool_call_end = {.id = "c1"}});
    feed_turn(&t, (struct stream_event){.kind = EV_DONE});

    struct agent_absorb_result absorbed = agent_session_absorb(&s, &t);
    turn_reset(&t);

    EXPECT(absorbed.items_from == 0);
    EXPECT(absorbed.had_tool_call);
    EXPECT(s.n_items == 1);
    EXPECT(s.items[0].kind == ITEM_TOOL_CALL);
    EXPECT_STR_EQ(s.items[0].tool_name, "bash");

    agent_session_free(&s);
}

static void test_session_context_snapshot(void)
{
    /* model/effort are owned (freed by agent_session_free), so seed
     * them with heap copies rather than string literals. */
    struct agent_session s = {
        .model = xstrdup("m1"),
        .effort = xstrdup("high"),
        .system_prompt = NULL,
        .tools = NULL,
        .n_tools = 0,
    };
    agent_session_add_user(&s, "go");

    struct context ctx = agent_session_context(&s);
    EXPECT(ctx.system_prompt == NULL);
    EXPECT(ctx.items == s.items);
    EXPECT(ctx.n_items == 2);
    EXPECT(ctx.tools == NULL);
    EXPECT(ctx.n_tools == 0);
    EXPECT_STR_EQ(ctx.effort, "high");

    agent_session_free(&s);
}

static struct stream_usage reported_usage(void)
{
    return (struct stream_usage){
        .input_tokens = 100,
        .output_tokens = 10,
        .cached_tokens = -1,
        .cache_write_tokens = -1,
        .cache_write_1h_tokens = -1,
        .cost = -1,
    };
}

static void test_mark_interrupt_skips_marked_result(void)
{
    struct agent_session session = {0};
    agent_session_append(&session,
                         (struct item){.kind = ITEM_TOOL_RESULT,
                                       .call_id = xstrdup("c1"),
                                       .output = xstrdup("partial output\n" INTERRUPT_MARKER)});
    struct stream_usage usage = reported_usage();
    agent_session_add_turn_usage(&session, NULL, &usage, 1000);

    size_t before = session.n_items;
    agent_session_mark_interrupt(&session);
    EXPECT(session.n_items == before);
    agent_session_free(&session);
}

static void test_mark_interrupt_marks_clean_result(void)
{
    struct agent_session session = {0};
    agent_session_append(&session, (struct item){.kind = ITEM_TOOL_RESULT,
                                                 .call_id = xstrdup("c2"),
                                                 .output = xstrdup("clean result")});
    struct stream_usage usage = reported_usage();
    agent_session_add_turn_usage(&session, NULL, &usage, 1000);
    agent_session_mark_interrupt(&session);

    EXPECT(session.n_items == 3);
    EXPECT(session.items[2].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT_STR_EQ(session.items[2].text, INTERRUPT_MARKER);
    agent_session_free(&session);
}

static void test_mark_interrupt_empty_session(void)
{
    struct agent_session session = {0};
    agent_session_mark_interrupt(&session);
    EXPECT(session.n_items == 1);
    EXPECT(session.items[0].kind == ITEM_ASSISTANT_MESSAGE);
    agent_session_free(&session);
}

int main(void)
{
    test_session_append();
    test_find_tool();
    test_build_system_prompt_raw();
    test_build_system_prompt_explicit_empty();
    test_build_system_prompt_custom_no_suffix();
    test_build_system_prompt_default_no_suffix();
    test_build_system_prompt_with_suffix();
    test_resolve_effort();
    test_recording_enabled();
    test_session_init_model_label();
    test_session_init_raw();
    test_session_init_missing_model();
    test_session_init_missing_provider();
    test_session_add_user();
    test_session_absorb_no_tool_call();
    test_session_absorb_with_tool_call();
    test_session_context_snapshot();
    test_mark_interrupt_skips_marked_result();
    test_mark_interrupt_marks_clean_result();
    test_mark_interrupt_empty_session();
    T_REPORT();
}
