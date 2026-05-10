/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "harness.h"
#include "tool.h"
#include "util.h"

/* Stand-in tool symbols for the link. agent_core's TOOLS[] references
 * &TOOL_READ / &TOOL_BASH / &TOOL_WRITE / &TOOL_EDIT, so the test binary
 * needs definitions to satisfy the linker. We fill in just enough for
 * find_tool's name lookup; .run is never invoked in these tests. */
static char *stub_run(const char *args, tool_emit_display_fn emit, void *user)
{
    (void)args;
    (void)emit;
    (void)user;
    return xstrdup("");
}

const struct tool TOOL_READ = {.def = {.name = "read"}, .run = stub_run};
const struct tool TOOL_BASH = {.def = {.name = "bash"}, .run = stub_run};
const struct tool TOOL_WRITE = {.def = {.name = "write"}, .run = stub_run};
const struct tool TOOL_EDIT = {.def = {.name = "edit"}, .run = stub_run};

/* ---------- items_append ---------- */

static void test_items_append_growth(void)
{
    struct item *items = NULL;
    size_t n = 0, cap = 0;

    /* Verify capacity doubling: empty → 16 → 32. We don't strictly need
     * to depend on the doubling factor (that's an internal detail), but
     * cap must always be >= n and growth must be amortized — i.e. cap
     * grows in steps, not on every append. */
    size_t prev_cap = 0;
    int cap_changes = 0;
    for (int i = 0; i < 40; i++) {
        items_append(&items, &n, &cap, (struct item){.kind = ITEM_USER_MESSAGE});
        if (cap != prev_cap) {
            cap_changes++;
            prev_cap = cap;
        }
    }
    EXPECT(n == 40);
    EXPECT(cap >= 40);
    EXPECT(cap_changes <= 5); /* a handful of grows, not 40 */

    for (size_t i = 0; i < n; i++)
        item_free(&items[i]);
    free(items);
}

static void test_items_append_preserves_payload(void)
{
    struct item *items = NULL;
    size_t n = 0, cap = 0;

    items_append(&items, &n, &cap,
                 (struct item){.kind = ITEM_USER_MESSAGE, .text = xstrdup("hello")});
    items_append(&items, &n, &cap, (struct item){.kind = ITEM_TURN_BOUNDARY});
    items_append(&items, &n, &cap,
                 (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup("world")});

    EXPECT(n == 3);
    EXPECT(items[0].kind == ITEM_USER_MESSAGE);
    EXPECT_STR_EQ(items[0].text, "hello");
    EXPECT(items[1].kind == ITEM_TURN_BOUNDARY);
    EXPECT(items[2].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT_STR_EQ(items[2].text, "world");

    for (size_t i = 0; i < n; i++)
        item_free(&items[i]);
    free(items);
}

/* ---------- find_tool ---------- */

static void test_find_tool(void)
{
    EXPECT(find_tool("read") == &TOOL_READ);
    EXPECT(find_tool("bash") == &TOOL_BASH);
    EXPECT(find_tool("write") == &TOOL_WRITE);
    EXPECT(find_tool("edit") == &TOOL_EDIT);
    EXPECT(find_tool("nonexistent") == NULL);
    EXPECT(find_tool("") == NULL);
}

/* ---------- build_system_prompt ---------- */

static void test_build_system_prompt_raw(void)
{
    /* raw=1 short-circuits: returns NULL no matter what HAX_SYSTEM_PROMPT
     * says or what the cwd contains. */
    setenv("HAX_SYSTEM_PROMPT", "ignored", 1);
    char *out = build_system_prompt("model-x", 1);
    EXPECT(out == NULL);
    unsetenv("HAX_SYSTEM_PROMPT");
}

static void test_build_system_prompt_explicit_empty(void)
{
    /* HAX_SYSTEM_PROMPT="" is the narrower opt-out: no system message
     * even when raw=0. */
    setenv("HAX_SYSTEM_PROMPT", "", 1);
    char *out = build_system_prompt("model-x", 0);
    EXPECT(out == NULL);
    unsetenv("HAX_SYSTEM_PROMPT");
}

static void test_build_system_prompt_custom_no_suffix(void)
{
    /* With env block + AGENTS.md suppressed, agent_env_build_suffix returns
     * NULL and the result is just the custom system prompt verbatim.
     * Lets us assert byte-equality without depending on cwd state. */
    setenv("HAX_SYSTEM_PROMPT", "you are a teapot", 1);
    setenv("HAX_NO_ENV", "1", 1);
    setenv("HAX_NO_AGENTS_MD", "1", 1);

    char *out = build_system_prompt("model-x", 0);
    EXPECT(out != NULL);
    if (out)
        EXPECT_STR_EQ(out, "you are a teapot");
    free(out);

    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
}

static void test_build_system_prompt_default_no_suffix(void)
{
    /* HAX_SYSTEM_PROMPT unset → DEFAULT_SYSTEM_PROMPT. We don't pin the
     * full text (which evolves), but the opening sentence is stable
     * enough to sanity-check we got the default and not a stub. */
    unsetenv("HAX_SYSTEM_PROMPT");
    setenv("HAX_NO_ENV", "1", 1);
    setenv("HAX_NO_AGENTS_MD", "1", 1);

    char *out = build_system_prompt("model-x", 0);
    EXPECT(out != NULL);
    if (out)
        EXPECT(strncmp(out, "You are hax", 11) == 0);
    free(out);

    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
}

static void test_build_system_prompt_with_suffix(void)
{
    /* When the env block is enabled, the result should start with the
     * custom prompt, then a blank-line separator, then the <env> block.
     * We don't pin the env contents (cwd/date/etc. drift) but the
     * structural shape is stable. */
    setenv("HAX_SYSTEM_PROMPT", "PREFIX", 1);
    setenv("HAX_NO_AGENTS_MD", "1", 1); /* suppress AGENTS.md content */
    unsetenv("HAX_NO_ENV");             /* keep <env> on */

    char *out = build_system_prompt("model-x", 0);
    EXPECT(out != NULL);
    if (out) {
        EXPECT(strncmp(out, "PREFIX\n\n", 8) == 0);
        EXPECT(strstr(out, "<env>") != NULL);
        EXPECT(strstr(out, "model: model-x") != NULL);
    }
    free(out);

    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_AGENTS_MD");
}

/* ---------- resolve_reasoning_effort ---------- */

static void test_resolve_reasoning_effort(void)
{
    struct provider p = {.default_reasoning_effort = "default-e"};

    /* unset → provider default */
    unsetenv("HAX_REASONING_EFFORT");
    EXPECT_STR_EQ(resolve_reasoning_effort(&p), "default-e");

    /* explicit empty → "force omit" (NULL), even though provider has a default */
    setenv("HAX_REASONING_EFFORT", "", 1);
    EXPECT(resolve_reasoning_effort(&p) == NULL);

    /* non-empty → passes through verbatim */
    setenv("HAX_REASONING_EFFORT", "low", 1);
    EXPECT_STR_EQ(resolve_reasoning_effort(&p), "low");

    /* with no provider default and unset env, returns NULL */
    unsetenv("HAX_REASONING_EFFORT");
    struct provider p2 = {.default_reasoning_effort = NULL};
    EXPECT(resolve_reasoning_effort(&p2) == NULL);
}

/* ---------- agent_session_init ---------- */

static void test_session_init_raw(void)
{
    /* --raw must produce sys=NULL, n_tools=0, tools=NULL even when
     * HAX_SYSTEM_PROMPT and HAX_MODEL would otherwise produce
     * substantive content. The model still resolves from env. */
    setenv("HAX_MODEL", "m-raw", 1);
    setenv("HAX_SYSTEM_PROMPT", "ignored", 1);

    struct provider p = {.name = "test", .default_model = NULL};
    struct hax_opts opts = {.raw = 1};

    struct agent_session s;
    EXPECT(agent_session_init(&s, &p, &opts) == 0);
    EXPECT(s.sys == NULL);
    EXPECT(s.tools == NULL);
    EXPECT(s.n_tools == 0);
    EXPECT_STR_EQ(s.model, "m-raw");

    /* Composed snapshot must match what the provider sees on the wire. */
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
    /* No HAX_MODEL and no provider default → init must fail with -1
     * and leave the session zeroed (so callers don't have to remember
     * to free a half-initialized session — main.c relies on this). */
    unsetenv("HAX_MODEL");
    struct provider p = {.name = "test", .default_model = NULL};
    struct hax_opts opts = {0};

    struct agent_session s;
    EXPECT(agent_session_init(&s, &p, &opts) == -1);
    EXPECT(s.items == NULL);
    EXPECT(s.tools == NULL);
    EXPECT(s.sys == NULL);
}

/* ---------- agent_session_add_user / add_boundary ---------- */

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

/* ---------- agent_session_absorb ---------- */

/* Drive a `struct turn` through an event sequence without going through
 * a provider, so we have a deterministic source of new items to absorb. */
static void feed(struct turn *t, struct stream_event ev)
{
    turn_on_event(&ev, t);
}

static void test_session_absorb_no_tool_call(void)
{
    struct agent_session s = {0};
    agent_session_add_user(&s, "go");

    struct turn t;
    turn_init(&t);
    feed(&t, (struct stream_event){.kind = EV_TEXT_DELTA, .u.text_delta = {.text = "answer"}});
    feed(&t, (struct stream_event){.kind = EV_DONE});

    size_t n_before = 999;
    int had_tc = 1;
    agent_session_absorb(&s, &t, &n_before, &had_tc);
    turn_reset(&t);

    EXPECT(n_before == 2); /* before this absorb, items had user+boundary */
    EXPECT(had_tc == 0);
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
    feed(&t, (struct stream_event){.kind = EV_TOOL_CALL_START,
                                   .u.tool_call_start = {.id = "c1", .name = "bash"}});
    feed(&t, (struct stream_event){.kind = EV_TOOL_CALL_DELTA,
                                   .u.tool_call_delta = {.id = "c1", .args_delta = "{}"}});
    feed(&t, (struct stream_event){.kind = EV_TOOL_CALL_END, .u.tool_call_end = {.id = "c1"}});
    feed(&t, (struct stream_event){.kind = EV_DONE});

    size_t n_before;
    int had_tc;
    agent_session_absorb(&s, &t, &n_before, &had_tc);
    turn_reset(&t);

    EXPECT(n_before == 0);
    EXPECT(had_tc == 1);
    EXPECT(s.n_items == 1);
    EXPECT(s.items[0].kind == ITEM_TOOL_CALL);
    EXPECT_STR_EQ(s.items[0].tool_name, "bash");

    agent_session_free(&s);
}

/* ---------- agent_session_context ---------- */

static void test_session_context_snapshot(void)
{
    struct agent_session s = {
        .model = "m1",
        .reasoning_effort = "high",
        .sys = NULL,
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
    EXPECT_STR_EQ(ctx.reasoning_effort, "high");

    agent_session_free(&s);
}

int main(void)
{
    test_items_append_growth();
    test_items_append_preserves_payload();
    test_find_tool();
    test_build_system_prompt_raw();
    test_build_system_prompt_explicit_empty();
    test_build_system_prompt_custom_no_suffix();
    test_build_system_prompt_default_no_suffix();
    test_build_system_prompt_with_suffix();
    test_resolve_reasoning_effort();
    test_session_init_raw();
    test_session_init_missing_model();
    test_session_add_user();
    test_session_absorb_no_tool_call();
    test_session_absorb_with_tool_call();
    test_session_context_snapshot();
    T_REPORT();
}
