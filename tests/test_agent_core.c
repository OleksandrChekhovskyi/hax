/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

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
    setenv("HAX_NO_SKILLS", "1", 1);
    setenv("HAX_NO_SUBAGENTS", "1", 1);

    char *out = build_system_prompt("model-x", 0);
    EXPECT(out != NULL);
    if (out)
        EXPECT_STR_EQ(out, "you are a teapot");
    free(out);

    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
    unsetenv("HAX_NO_SKILLS");
    unsetenv("HAX_NO_SUBAGENTS");
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

/* ---------- resolve_effort ---------- */

static const char *const test_effort_levels[] = {"low", "high"};

static size_t test_list_efforts(struct provider *p, const char *const **out)
{
    (void)p;
    *out = test_effort_levels;
    return 2;
}

static void test_resolve_effort(void)
{
    struct provider p = {.default_effort = "default-e", .list_efforts = test_list_efforts};

    /* unset → provider default */
    unsetenv("HAX_EFFORT");
    EXPECT_STR_EQ(resolve_effort(&p), "default-e");

    /* explicit empty → "force omit" (NULL), even though provider has a default */
    setenv("HAX_EFFORT", "", 1);
    EXPECT(resolve_effort(&p) == NULL);

    /* non-empty → passes through verbatim */
    setenv("HAX_EFFORT", "low", 1);
    EXPECT_STR_EQ(resolve_effort(&p), "low");

    /* with no provider default and unset env, returns NULL */
    unsetenv("HAX_EFFORT");
    struct provider p2 = {.default_effort = NULL, .list_efforts = test_list_efforts};
    EXPECT(resolve_effort(&p2) == NULL);

    /* a provider with no effort ladder (NULL hook, or one that reports zero
     * levels) never resolves an effort — even one persisted in config — so a
     * stale value can't leak onto e.g. llama.cpp / ollama. */
    setenv("HAX_EFFORT", "high", 1);
    struct provider p3 = {.default_effort = "default-e", .list_efforts = NULL};
    EXPECT(resolve_effort(&p3) == NULL);

    /* a value the provider's ladder doesn't accept (a stale pick carried over
     * from a different backend) falls back to the provider default rather than
     * being sent verbatim. test_list_efforts offers {low, high}. */
    setenv("HAX_EFFORT", "medium", 1);
    EXPECT_STR_EQ(resolve_effort(&p), "default-e");
    /* same, but no provider default to fall back to → omit. */
    EXPECT(resolve_effort(&p2) == NULL);
    unsetenv("HAX_EFFORT");
}

/* ---------- agent_session_init ---------- */

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
    EXPECT(agent_session_init(&s, &p, &opts) == 0);
    EXPECT_STR_EQ(s.model, "/models/long-model.gguf");
    EXPECT_STR_EQ(s.model_label, "short-model");
    EXPECT(s.sys != NULL && strstr(s.sys, "model: short-model") != NULL);
    EXPECT(s.sys != NULL && strstr(s.sys, "/models/long-model.gguf") == NULL);

    agent_session_free(&s);
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_AGENTS_MD");
}

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
    /* No HAX_MODEL and no provider default: init now succeeds with an
     * empty model rather than failing, so the interactive REPL can start
     * and prompt the user to pick one at runtime (/model, /provider). The
     * one-shot path checks the empty model itself and fails fast there. */
    unsetenv("HAX_MODEL");
    struct provider p = {.name = "test", .default_model = NULL};
    struct hax_opts opts = {0};

    struct agent_session s;
    EXPECT(agent_session_init(&s, &p, &opts) == 0);
    EXPECT(s.model == NULL || s.model[0] == '\0');
    agent_session_free(&s);
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
    /* model/effort are owned (freed by agent_session_free), so seed
     * them with heap copies rather than string literals. */
    struct agent_session s = {
        .model = xstrdup("m1"),
        .effort = xstrdup("high"),
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
    EXPECT_STR_EQ(ctx.effort, "high");

    agent_session_free(&s);
}

static void test_format_stats_segments_selection(void)
{
    char segs[STATS_SEGS_MAX][STATS_SEG_LEN];

    /* Default (non-verbose): duration, context gauge, spend — scope order,
     * this turn before session state; out/cached dropped, and the single
     * token figure carries no word label. */
    int n = format_stats_segments(segs, 9113, 262144, 595, 2765, 0, 42000, 0.042, 0);
    EXPECT(n == 3);
    EXPECT_STR_EQ(segs[0], "42s");
    EXPECT_STR_EQ(segs[1], "8.9k / 256k (3%)");
    EXPECT_STR_EQ(segs[2], "$0.042");

    /* Verbose: out and cached slot in, and every field gets its word
     * label (the fully labeled diagnostic form). */
    n = format_stats_segments(segs, 9113, 262144, 595, 2765, 1, 42000, 0.042, 0);
    EXPECT(n == 5);
    EXPECT_STR_EQ(segs[0], "worked 42s");
    EXPECT_STR_EQ(segs[1], "out 595");
    EXPECT_STR_EQ(segs[2], "context 8.9k / 256k (3%)");
    EXPECT_STR_EQ(segs[3], "cached 2.7k");
    EXPECT_STR_EQ(segs[4], "spent $0.042");

    /* Unknown window: no gauge shape to identify the bare number, so the
     * default form keeps the "context" label for this figure only. */
    n = format_stats_segments(segs, 9113, 0, -1, -1, 0, 42000, 0.042, 0);
    EXPECT(n == 3);
    EXPECT_STR_EQ(segs[0], "42s");
    EXPECT_STR_EQ(segs[1], "context 8.9k");
    EXPECT_STR_EQ(segs[2], "$0.042");

    /* Estimated spend is marked approximate, both forms. */
    n = format_stats_segments(segs, -1, 0, -1, -1, 0, -1, 0.042, 1);
    EXPECT(n == 1);
    EXPECT_STR_EQ(segs[0], "~$0.042");
    n = format_stats_segments(segs, -1, 0, -1, -1, 1, -1, 0.042, 1);
    EXPECT(n == 1);
    EXPECT_STR_EQ(segs[0], "spent ~$0.042");

    /* Unreported fields are skipped: no usage, no cost ⇒ duration only
     * (labeled, since this asks for the verbose form). */
    n = format_stats_segments(segs, -1, 0, -1, -1, 1, 42000, 0, 0);
    EXPECT(n == 1);
    EXPECT_STR_EQ(segs[0], "worked 42s");

    /* Nothing reported at all. */
    n = format_stats_segments(segs, -1, 0, -1, -1, 0, -1, 0, 0);
    EXPECT(n == 0);
}

static void test_spend_accounting(void)
{
    struct spend_totals t = {0};

    /* Reported cost is exact: sums into reported, no segment tokens. */
    struct stream_usage u = {.input_tokens = 1000,
                             .output_tokens = 50,
                             .cached_tokens = 200,
                             .cache_write_tokens = -1,
                             .cache_write_1h_tokens = -1,
                             .cost = 0.01};
    spend_account(&t, &u);
    EXPECT(t.reported == 0.01);
    EXPECT(t.seg_input == 0 && t.seg_output == 0);

    /* Unreported cost: token counts land in the open segment; -1 ("not
     * reported") fields don't contribute. */
    u.cost = -1;
    spend_account(&t, &u);
    spend_account(&t, &u);
    EXPECT(t.reported == 0.01);
    EXPECT(t.seg_input == 2000);
    EXPECT(t.seg_output == 100);
    EXPECT(t.seg_cached == 400);
    EXPECT(t.seg_cache_write == 0);
    EXPECT(t.seg_cache_write_1h == 0);

    /* Cache writes and their 1h subset accumulate like the others. */
    u.cache_write_tokens = 300;
    u.cache_write_1h_tokens = 120;
    spend_account(&t, &u);
    EXPECT(t.seg_cache_write == 300);
    EXPECT(t.seg_cache_write_1h == 120);
    u.cache_write_tokens = -1;
    u.cache_write_1h_tokens = -1;

    /* An explicit zero cost is a *reported* free response, not "cost
     * unknown": nothing may land in the segment, where catalog rates
     * would later re-price the free tokens as paid. */
    struct spend_totals z = {0};
    u.cost = 0;
    spend_account(&z, &u);
    EXPECT(z.reported == 0);
    EXPECT(z.seg_input == 0 && z.seg_output == 0 && z.seg_cached == 0);
    u.cost = -1;

    /* Folding sums every field. */
    struct spend_totals sum = {.reported = 1.0, .seg_input = 5};
    spend_fold(&sum, &t);
    EXPECT(sum.reported == 1.01);
    EXPECT(sum.seg_input == 3005);
    EXPECT(sum.seg_output == 150);
    EXPECT(sum.seg_cache_write == 300);
    EXPECT(sum.seg_cache_write_1h == 120);

    /* spend_has_tokens: any segment tokens count; reported cost doesn't. */
    EXPECT(spend_has_tokens(&t));
    EXPECT(!spend_has_tokens(&z));

    /* Unpriceable segments: no tokens, no provider, or no catalog_id. */
    struct spend_totals empty = {.reported = 9.0};
    struct provider p = {.name = "x"};
    EXPECT(spend_estimate(&empty, &p, "m") == -1);
    EXPECT(spend_estimate(&t, NULL, "m") == -1);
    EXPECT(spend_estimate(&t, &p, "m") == -1); /* catalog_id unset */

    /* The positive path: a mapped provider whose model resolves in the
     * catalog prices the segment (fixture snapshot, real catalog module). */
    char dir[] = "/tmp/hax_test_agent_core_XXXXXX";
    if (!mkdtemp(dir))
        FAIL("mkdtemp: %s", strerror(errno));
    setenv("XDG_CACHE_HOME", dir, 1);
    char path[600];
    snprintf(path, sizeof(path), "%s/hax", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/hax/catalog.json", dir);
    FILE *f = fopen(path, "w");
    EXPECT(f != NULL);
    if (f) {
        fputs("{\"prov\": {\"models\": {\"m\": {\"cost\": {\"input\": 2, \"output\": 8}}}}}", f);
        fclose(f);
    }
    p.catalog_id = "prov";
    struct spend_totals seg = {.seg_input = 1000000, .seg_output = 1000000};
    EXPECT(spend_estimate(&seg, &p, "m") == 10.0);
    EXPECT(spend_estimate(&seg, &p, "unknown-model") == -1);
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
    test_resolve_effort();
    test_session_init_model_label();
    test_session_init_raw();
    test_session_init_missing_model();
    test_session_add_user();
    test_session_absorb_no_tool_call();
    test_session_absorb_with_tool_call();
    test_session_context_snapshot();
    test_format_stats_segments_selection();
    test_spend_accounting();
    T_REPORT();
}
