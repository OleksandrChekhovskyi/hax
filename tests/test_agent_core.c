/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "agent_core.h"
#include "catalog.h"
#include "harness.h"
#include "tool.h"
#include "util.h"

/* Stand-in tool symbols for the link. agent_core's TOOLS[] references
 * &TOOL_READ / &TOOL_EDIT / &TOOL_WRITE / &TOOL_BASH, so the test binary
 * needs definitions to satisfy the linker. We fill in just enough for
 * find_tool's name lookup; .run is never invoked in these tests. */
static char *stub_run(const char *args, struct tool_ctx *ctx)
{
    (void)args;
    (void)ctx;
    return xstrdup("");
}

const struct tool TOOL_READ = {.def = {.name = "read"}, .run = stub_run};
const struct tool TOOL_EDIT = {.def = {.name = "edit"}, .run = stub_run};
const struct tool TOOL_WRITE = {.def = {.name = "write"}, .run = stub_run};
const struct tool TOOL_BASH = {.def = {.name = "bash"}, .run = stub_run};

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
    /* With the Environment section + AGENTS.md suppressed, agent_env_build_suffix returns
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
    /* When the Environment section is enabled, the result should start with
     * the custom prompt, then a blank-line separator, then its Markdown
     * heading. We don't pin the dynamic contents, but the shape is stable. */
    setenv("HAX_SYSTEM_PROMPT", "PREFIX", 1);
    setenv("HAX_NO_AGENTS_MD", "1", 1); /* suppress AGENTS.md content */
    unsetenv("HAX_NO_ENV");             /* keep Environment on */

    char *out = build_system_prompt("model-x", 0);
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
    EXPECT(s.sys != NULL && strstr(s.sys, "- Model: short-model") != NULL);
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

static void test_mark_interrupt(void)
{
    struct stream_usage u = {.input_tokens = 100,
                             .output_tokens = 10,
                             .cached_tokens = -1,
                             .cache_write_tokens = -1,
                             .cache_write_1h_tokens = -1,
                             .cost = -1};

    /* The regression case: a tool result already marked at the abort
     * boundary (bash's own "[interrupted]" footer), with the round-trip's
     * usage footer appended after it. The marker check must look past the
     * inert footer, see the marked result, and add no duplicate. */
    struct agent_session s = {0};
    items_append(&s.items, &s.n_items, &s.cap_items,
                 (struct item){.kind = ITEM_TOOL_RESULT,
                               .call_id = xstrdup("c1"),
                               .output = xstrdup("partial output\n" INTERRUPT_MARKER)});
    agent_session_add_turn_usage(&s, NULL, &u, 1000);
    EXPECT(s.n_items == 2);
    size_t before = s.n_items;
    agent_session_mark_interrupt(&s);
    EXPECT(s.n_items == before);
    agent_session_free(&s);

    /* A clean (unmarked) result behind the footer still gets the
     * synthetic assistant marker. */
    struct agent_session s2 = {0};
    items_append(&s2.items, &s2.n_items, &s2.cap_items,
                 (struct item){.kind = ITEM_TOOL_RESULT,
                               .call_id = xstrdup("c2"),
                               .output = xstrdup("clean result")});
    agent_session_add_turn_usage(&s2, NULL, &u, 1000);
    agent_session_mark_interrupt(&s2);
    EXPECT(s2.n_items == 3);
    if (s2.n_items == 3) {
        EXPECT(s2.items[2].kind == ITEM_ASSISTANT_MESSAGE);
        EXPECT_STR_EQ(s2.items[2].text, INTERRUPT_MARKER);
    }
    agent_session_free(&s2);

    /* Empty history: the marker is the whole record of the abort. */
    struct agent_session s3 = {0};
    agent_session_mark_interrupt(&s3);
    EXPECT(s3.n_items == 1);
    EXPECT(s3.n_items == 1 && s3.items[0].kind == ITEM_ASSISTANT_MESSAGE);
    agent_session_free(&s3);
}

static void test_format_stats_segments_selection(void)
{
    char segs[STATS_SEGS_MAX][STATS_SEG_LEN];

    /* Duration, context gauge, spend — scope order, this turn before
     * session state; the single token figure carries no word label. */
    int n = format_stats_segments(segs, 9113, 262144, 42000, 0.042, 0);
    EXPECT(n == 3);
    EXPECT_STR_EQ(segs[0], "42s");
    EXPECT_STR_EQ(segs[1], "8.9k / 256k (3%)");
    EXPECT_STR_EQ(segs[2], "$0.042");

    /* Unknown window: no gauge shape to identify the bare number, so the
     * "context" label sticks to this figure only. */
    n = format_stats_segments(segs, 9113, 0, 42000, 0.042, 0);
    EXPECT(n == 3);
    EXPECT_STR_EQ(segs[0], "42s");
    EXPECT_STR_EQ(segs[1], "context 8.9k");
    EXPECT_STR_EQ(segs[2], "$0.042");

    /* Estimated spend is marked approximate. */
    n = format_stats_segments(segs, -1, 0, -1, 0.042, 1);
    EXPECT(n == 1);
    EXPECT_STR_EQ(segs[0], "~$0.042");

    /* Unreported fields are skipped: no usage, no cost ⇒ duration only. */
    n = format_stats_segments(segs, -1, 0, 42000, 0, 0);
    EXPECT(n == 1);
    EXPECT_STR_EQ(segs[0], "42s");

    /* Nothing reported at all. */
    n = format_stats_segments(segs, -1, 0, -1, 0, 0);
    EXPECT(n == 0);
}

static void test_spend_accounting(void)
{
    struct spend_totals t = {0};
    int approx = 0;

    /* Fixture snapshot first (the catalog memoizes misses per
     * provider/model, so no lookup may precede the write). */
    char *dir = t_tempdir();
    setenv("XDG_CACHE_HOME", dir, 1);
    char path[600];
    snprintf(path, sizeof(path), "%s/hax", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/hax/catalog.json", dir);
    FILE *f = fopen(path, "w");
    EXPECT(f != NULL);
    if (f) {
        fputs("{\"prov\": {\"models\": {"
              "\"m\": {\"cost\": {\"input\": 2, \"output\": 8}},"
              "\"free-m\": {\"cost\": {\"input\": 0, \"output\": 0}}"
              "}}}",
              f);
        fclose(f);
    }

    /* Reported cost is exact: sums into reported, no record kept. */
    struct stream_usage u = {.input_tokens = 1000,
                             .output_tokens = 50,
                             .cached_tokens = 200,
                             .cache_write_tokens = -1,
                             .cache_write_1h_tokens = -1,
                             .cost = 0.01};
    spend_account(&t, &u, "prov", "m");
    EXPECT(t.reported == 0.01);
    EXPECT(t.n_recs == 0);
    EXPECT(spend_total(&t, &approx) == 0.01);
    EXPECT(!approx);

    /* Unreported cost: each response becomes its own stamped record.
     * Neither stamp resolves in the catalog, so the total stays at the
     * reported subtotal, marked approximate — real usage exists that the
     * figure doesn't cover. */
    u.cost = -1;
    spend_account(&t, &u, "noprov", "m");
    spend_account(&t, &u, NULL, NULL);
    EXPECT(t.reported == 0.01);
    EXPECT(t.n_recs == 2);
    EXPECT_STR_EQ(t.recs[0].catalog_id, "noprov");
    EXPECT_STR_EQ(t.recs[0].model, "m");
    EXPECT(t.recs[1].catalog_id == NULL && t.recs[1].model == NULL);
    EXPECT(spend_unpriced(&t));
    EXPECT(spend_total(&t, &approx) == 0.01);
    EXPECT(approx);

    /* A response that reported neither cost nor tokens records nothing. */
    struct stream_usage nothing = {-1, -1, -1, -1, -1, -1};
    spend_account(&t, &nothing, "prov", "m");
    EXPECT(t.n_recs == 2);

    /* An explicit zero cost is a *reported* free response, not "cost
     * unknown": no record may be kept, where catalog rates would later
     * re-price the free tokens as paid. */
    struct spend_totals z = {0};
    u.cost = 0;
    spend_account(&z, &u, "prov", "m");
    EXPECT(z.reported == 0);
    EXPECT(z.n_recs == 0);
    EXPECT(spend_total(&z, &approx) == 0);
    EXPECT(!approx);
    u.cost = -1;

    /* The positive path: a stamp the catalog resolves prices its record
     * into the total (fixture snapshot, real catalog module) — still
     * approximate, being an estimate. */
    struct spend_totals big = {0};
    struct stream_usage mega = {.input_tokens = 1000000,
                                .output_tokens = 1000000,
                                .cached_tokens = -1,
                                .cache_write_tokens = -1,
                                .cache_write_1h_tokens = -1,
                                .cost = -1};
    spend_account(&big, &mega, "prov", "m");
    EXPECT(spend_total(&big, &approx) == 10.0);
    EXPECT(approx);
    EXPECT(!spend_unpriced(&big));
    struct spend_totals miss = {0};
    spend_account(&miss, &mega, "prov", "unknown-model");
    EXPECT(spend_total(&miss, &approx) == 0);
    EXPECT(approx);
    EXPECT(spend_unpriced(&miss));

    /* spend_split: the estimated spend broken down per category (the
     * /session row), summed across priceable records only. */
    struct catalog_split sp;
    EXPECT(spend_split(&big, &sp) == 1);
    EXPECT(sp.in == 2.0 && sp.out == 8.0);
    EXPECT(sp.cache_read == 0 && sp.cache_write == 0);
    EXPECT(spend_split(&miss, &sp) == 0);

    /* Zero catalog rates price a record to $0 — still an estimate, so
     * the figure stays approximate rather than passing off the reported
     * subtotal as an exact grand total. */
    struct spend_totals freebie = {.reported = 0.5};
    spend_account(&freebie, &mega, "prov", "free-m");
    EXPECT(spend_total(&freebie, &approx) == 0.5);
    EXPECT(approx);
    EXPECT(!spend_unpriced(&freebie));
    spend_free(&freebie);

    spend_free(&t);
    spend_free(&z);
    spend_free(&big);
    spend_free(&miss);
    EXPECT(t.n_recs == 0 && t.recs == NULL);
}

static void test_turn_usage_make(void)
{
    /* Reported cost: exact total, never decomposed. */
    struct stream_usage u = {.input_tokens = 1000,
                             .output_tokens = 50,
                             .cached_tokens = 200,
                             .cache_write_tokens = -1,
                             .cache_write_1h_tokens = -1,
                             .cost = 0.01};
    struct turn_usage *tu = turn_usage_make(&u, 1500, "prov", "m");
    EXPECT(tu != NULL);
    if (tu) {
        EXPECT(tu->cost_total == 0.01);
        EXPECT(!tu->cost_estimated);
        EXPECT(tu->cost_in < 0 && tu->cost_out < 0);
        EXPECT(tu->elapsed_ms == 1500);
        free(tu);
    }

    /* No usage but a known duration: a duration-only footer, so
     * usage-less backends still document each successful round-trip.
     * With no duration either there is nothing to show — NULL. */
    struct stream_usage nothing = {-1, -1, -1, -1, -1, -1};
    EXPECT(!usage_reported(&nothing));
    tu = turn_usage_make(&nothing, 1500, "prov", "m");
    EXPECT(tu != NULL);
    if (tu) {
        EXPECT(tu->elapsed_ms == 1500);
        EXPECT(tu->cost_total < 0);
        EXPECT(!tu->cost_estimated);
        free(tu);
    }
    EXPECT(turn_usage_make(&nothing, -1, "prov", "m") == NULL);

    /* Unreported cost against the fixture catalog (written by
     * test_spend_accounting, still in XDG_CACHE_HOME): estimated total
     * with the per-category split. 500k uncached in ($1) + 500k cached
     * reads at the input-rate fallback ($1) + 1M out ($8). */
    struct stream_usage est = {.input_tokens = 1000000,
                               .output_tokens = 1000000,
                               .cached_tokens = 500000,
                               .cache_write_tokens = -1,
                               .cache_write_1h_tokens = -1,
                               .cost = -1};
    tu = turn_usage_make(&est, -1, "prov", "m");
    EXPECT(tu != NULL);
    if (tu) {
        EXPECT(tu->cost_estimated);
        EXPECT(tu->cost_total == 10.0);
        EXPECT(tu->cost_in == 1.0);
        EXPECT(tu->cost_cache_read == 1.0);
        EXPECT(tu->cost_cache_write == 0);
        EXPECT(tu->cost_out == 8.0);
        free(tu);
    }

    /* No catalog identity: raw usage carries through, costs unknown. */
    tu = turn_usage_make(&est, 2000, NULL, NULL);
    EXPECT(tu != NULL);
    if (tu) {
        EXPECT(tu->cost_total < 0);
        EXPECT(!tu->cost_estimated);
        EXPECT(tu->usage.input_tokens == 1000000);
        free(tu);
    }
}

static void test_agent_image_input_resolution(void)
{
    struct provider p = {.name = "x"};

    /* No probe answer, no catalog identity: unknown. */
    EXPECT(agent_image_input(&p, "m") == -1);
    EXPECT(agent_image_input(NULL, NULL) == -1);

    /* A live probe answer decides. */
    atomic_store(&p.image_input, PROVIDER_IMG_YES);
    EXPECT(agent_image_input(&p, "m") == 1);
    atomic_store(&p.image_input, PROVIDER_IMG_NO);
    EXPECT(agent_image_input(&p, "m") == 0);

    /* The config tristate pins the answer over any probe result; "auto"
     * (the default) falls through to detection. */
    setenv("HAX_IMAGE_INPUT", "on", 1);
    EXPECT(agent_image_input(&p, "m") == 1);
    setenv("HAX_IMAGE_INPUT", "off", 1);
    atomic_store(&p.image_input, PROVIDER_IMG_YES);
    EXPECT(agent_image_input(&p, "m") == 0);
    setenv("HAX_IMAGE_INPUT", "auto", 1);
    EXPECT(agent_image_input(&p, "m") == 1);
    unsetenv("HAX_IMAGE_INPUT");
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
    test_mark_interrupt();
    test_format_stats_segments_selection();
    test_spend_accounting();
    test_turn_usage_make();
    test_agent_image_input_resolution();
    T_REPORT();
}
