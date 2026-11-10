/* SPDX-License-Identifier: MIT */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "agent_core.h"
#include "agent_stats.h"
#include "harness.h"
#include "model_meta.h"
#include "provider.h"
#include "xalloc.h"

/* Catalog misses are memoized, so install the fixture before any pricing call. */
static void install_catalog(void)
{
    char *dir = t_tempdir();
    setenv("XDG_CACHE_HOME", dir, 1);

    char path[600];
    snprintf(path, sizeof(path), "%s/hax", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/hax/catalog.json", dir);
    FILE *file = fopen(path, "w");
    EXPECT(file != NULL);
    if (!file)
        return;
    fputs("{\"prov\": {\"models\": {\"m\": {\"cost\": {\"input\": 2, \"output\": 8}}}}}", file);
    fclose(file);
}

static struct stream_usage tokens(long input, long output, double cost)
{
    return (struct stream_usage){input, output, -1, -1, -1, cost};
}

static void add_prompt(struct agent_session *session, const char *text)
{
    agent_session_add_user(session, text);
    agent_session_append(session,
                         (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup("reply")});
}

static void add_tool_call(struct agent_session *session, const char *name)
{
    agent_session_append(session, (struct item){.kind = ITEM_TOOL_CALL,
                                                .call_id = xstrdup("c"),
                                                .tool_name = xstrdup(name),
                                                .tool_arguments_json = xstrdup("{}")});
}

/* A usage record as a provider without rates leaves it: tokens and maybe a reported charge, no
 * category split. */
static void add_usage_record(struct agent_session *session, const char *provider, const char *model,
                             struct stream_usage usage, double cost_total, int estimated)
{
    struct turn_usage *record = xcalloc(1, sizeof(*record));
    record->usage = usage;
    record->elapsed_ms = 100;
    record->uncached_input_tokens = usage.input_tokens > 0 ? usage.input_tokens : 0;
    record->cost_input = -1;
    record->cost_cache_read = -1;
    record->cost_cache_write = -1;
    record->cost_output = -1;
    record->cost_total = cost_total;
    record->cost_estimated = estimated;
    agent_session_append(session, (struct item){.kind = ITEM_TURN_USAGE,
                                                .usage = record,
                                                .provider = xstrdup(provider),
                                                .model = xstrdup(model)});
}

static void test_counts_user_turns_tools_and_requests(void)
{
    struct agent_session session = {0};
    add_prompt(&session, "one");
    add_tool_call(&session, "bash");
    add_tool_call(&session, "read");
    add_usage_record(&session, "prov", "m", tokens(1000, 100, 0.01), 0.01, 0);
    add_prompt(&session, "two");
    add_tool_call(&session, "bash");
    add_usage_record(&session, "prov", "m", tokens(2000, 200, 0.02), 0.02, 0);
    agent_session_add_worked(&session, 4000);
    agent_session_add_worked(&session, 6000);

    struct agent_stats stats;
    agent_stats_collect(&session, 0, 0, NULL, &stats);
    EXPECT(stats.user_turns == 2);
    EXPECT(stats.undone_user_turns == 0);
    EXPECT(stats.tool_calls == 3);
    EXPECT_STR_EQ(stats.tools[0].name, "bash");
    EXPECT(stats.tools[0].count == 2);
    EXPECT_STR_EQ(stats.tools[1].name, "read");
    EXPECT(stats.tools[1].count == 1);
    EXPECT(stats.worked_ms == 10000);
    EXPECT(stats.context_tokens == 2200);
    EXPECT(stats.total.requests == 2);
    EXPECT(stats.total.input_tokens == 3000);
    EXPECT(stats.total.output_tokens == 300);
    EXPECT(stats.total.spend == 0.03);
    EXPECT(!stats.total.spend_estimated);
    EXPECT(!stats.total.unpriced);
    EXPECT(stats.n_models == 1);
    EXPECT_STR_EQ(stats.models[0].model, "m");

    /* A range excludes earlier history but never the conversation's own usage records after it. */
    agent_stats_collect(&session, 6, 0, NULL, &stats);
    EXPECT(stats.user_turns == 1);
    EXPECT(stats.total.requests == 1);
    EXPECT(stats.total.spend == 0.02);
    agent_session_free(&session);
}

static void test_retired_items_stay_on_the_bill(void)
{
    struct agent_session session = {0};
    add_prompt(&session, "kept");
    add_usage_record(&session, "prov", "m", tokens(1000, 100, 0.01), 0.01, 0);
    add_prompt(&session, "undone");
    add_tool_call(&session, "bash");
    add_usage_record(&session, "prov", "m", tokens(2000, 200, 0.02), 0.02, 0);
    agent_session_retire(&session, items_user_turn_cut(session.items, session.n_items, 1));

    /* The conversation shrank to one user turn and no tool calls; the bill did not. */
    struct agent_stats stats;
    agent_stats_collect(&session, 0, 0, NULL, &stats);
    EXPECT(stats.user_turns == 1);
    EXPECT(stats.undone_user_turns == 1);
    EXPECT(stats.tool_calls == 0);
    EXPECT(stats.total.requests == 2);
    EXPECT(stats.total.spend == 0.03);
    /* The window snapshot describes the live tail only. */
    EXPECT(stats.context_tokens == 1100);

    /* A run that started after the undo owns neither the kept history nor the undone turn. */
    agent_stats_collect(&session, session.n_items, session.n_retired, NULL, &stats);
    EXPECT(stats.total.requests == 0);
    EXPECT(stats.total.spend == 0);
    EXPECT(stats.undone_user_turns == 0);
    agent_session_free(&session);
}

static void test_models_are_split_by_record_identity(void)
{
    struct agent_session session = {0};
    add_prompt(&session, "a");
    add_usage_record(&session, "prov", "small", tokens(1000, 100, 0.01), 0.01, 0);
    add_prompt(&session, "b");
    add_usage_record(&session, "prov", "large", tokens(2000, 200, 0.10), 0.10, 0);
    add_prompt(&session, "c");
    add_usage_record(&session, "prov", "large", tokens(3000, 300, 0.20), 0.20, 0);
    /* Labels stored with the usage record name the row, not the wire id. */
    session.items[session.n_items - 1].usage->provenance.model_label = xstrdup("Large");

    struct agent_stats stats;
    agent_stats_collect(&session, 0, 0, NULL, &stats);
    EXPECT(stats.n_models == 3);
    EXPECT_STR_EQ(stats.models[0].model, "small");
    EXPECT(stats.models[0].totals.spend == 0.01);
    EXPECT_STR_EQ(stats.models[1].model, "large");
    EXPECT(stats.models[1].totals.input_tokens == 2000);
    EXPECT_STR_EQ(stats.models[2].model, "Large");
    EXPECT(stats.total.spend == 0.31);
    agent_session_free(&session);
}

static void test_estimates_and_unpriced_records_mark_spend(void)
{
    install_catalog();
    struct agent_session session = {0};
    add_usage_record(&session, "prov", "m", tokens(1000, 50, 0.01), 0.01, 0);
    /* Recorded as an estimate by the usage record itself. */
    add_usage_record(&session, "prov", "m", tokens(1000, 50, -1), 0.02, 1);
    struct agent_stats stats;
    agent_stats_collect(&session, 0, 0, NULL, &stats);
    EXPECT(stats.total.spend == 0.03);
    EXPECT(stats.total.spend_estimated);
    EXPECT(!stats.total.unpriced);

    /* Recorded before rates were known: priced lazily from the catalog through the live
     * provider, and the estimate splits into categories. */
    struct provider provider = {.name = "prov", .catalog_id = "prov"};
    setenv("HAX_PROVIDER", "prov", 1);
    add_usage_record(&session, provider_stable_id(&provider), "m", tokens(1000000, 1000000, -1), -1,
                     0);
    agent_stats_collect(&session, 0, 0, &provider, &stats);
    EXPECT(stats.total.spend == 10.03);
    EXPECT(stats.total.spend_estimated);
    EXPECT(!stats.total.unpriced);
    EXPECT(stats.total.split_available);
    /* Category estimates cover the reported usage records too, at the same rates. */
    EXPECT(fabs(stats.total.split.cost_input - 2.004) < 1e-9);
    EXPECT(fabs(stats.total.split.cost_output - 8.0008) < 1e-9);

    /* A response that reported neither tokens nor cost has nothing to price: it is a request,
     * not a gap in the estimate, so no caller waits on the catalog for it. */
    add_usage_record(&session, "prov", "m", tokens(-1, -1, -1), -1, 0);
    agent_stats_collect(&session, 0, 0, &provider, &stats);
    EXPECT(stats.total.requests == 4);
    EXPECT(!stats.total.unpriced);

    /* A usage record nothing can price is missing real usage: the total stays a lower bound. */
    add_usage_record(&session, "elsewhere", "unknown", tokens(500, 5, -1), -1, 0);
    agent_stats_collect(&session, 0, 0, &provider, &stats);
    EXPECT(stats.total.spend == 10.03);
    EXPECT(stats.total.unpriced);
    unsetenv("HAX_PROVIDER");
    model_meta_release(&provider);
    agent_session_free(&session);
}

static void test_inherited_items_are_context_not_bill(void)
{
    struct agent_session session = {0};
    add_prompt(&session, "parent");
    add_usage_record(&session, "prov", "m", tokens(1000, 100, 0.05), 0.05, 0);
    for (size_t i = 0; i < session.n_items; i++)
        session.items[i].inherited = 1;
    add_prompt(&session, "own");
    add_usage_record(&session, "prov", "m", tokens(2000, 200, 0.02), 0.02, 0);

    struct agent_stats stats;
    agent_stats_collect(&session, 0, 0, NULL, &stats);
    EXPECT(stats.inherited_user_turns == 1);
    EXPECT(stats.inherited.spend == 0.05);
    EXPECT(stats.user_turns == 1);
    EXPECT(stats.total.requests == 1);
    EXPECT(stats.total.spend == 0.02);
    /* Context still counts the inherited prefix: the model reads it either way. */
    EXPECT(stats.context_tokens == 2200);

    /* Undoing back into the inherited prefix drops those user turns from both views. */
    agent_session_retire(&session, items_user_turn_cut(session.items, session.n_items, 0));
    agent_stats_collect(&session, 0, 0, NULL, &stats);
    EXPECT(stats.inherited_user_turns == 0);
    EXPECT(stats.inherited.spend == 0);
    EXPECT(stats.user_turns == 0);
    EXPECT(stats.undone_user_turns == 1);
    EXPECT(stats.total.spend == 0.02);
    agent_session_free(&session);
}

static void test_format_stats_segments(void)
{
    char segments[AGENT_STATS_MAX_SEGMENTS][AGENT_STATS_SEGMENT_LEN];

    int count = agent_format_stats_segments(segments, 9113, 262144, 42000, 0.042, 0);
    EXPECT(count == 3);
    EXPECT_STR_EQ(segments[0], "42s");
    EXPECT_STR_EQ(segments[1], "9.1k / 262k (3%)");
    EXPECT_STR_EQ(segments[2], "$0.042");

    count = agent_format_stats_segments(segments, 9113, 0, 42000, 0.042, 0);
    EXPECT(count == 3);
    EXPECT_STR_EQ(segments[1], "context 9.1k");

    count = agent_format_stats_segments(segments, -1, 0, -1, 0.042, 1);
    EXPECT(count == 1);
    EXPECT_STR_EQ(segments[0], "~$0.042");

    count = agent_format_stats_segments(segments, -1, 0, 42000, 0, 0);
    EXPECT(count == 1);
    EXPECT_STR_EQ(segments[0], "42s");

    EXPECT(agent_format_stats_segments(segments, -1, 0, -1, 0, 0) == 0);
}

int main(void)
{
    test_format_stats_segments();
    test_counts_user_turns_tools_and_requests();
    test_retired_items_stay_on_the_bill();
    test_models_are_split_by_record_identity();
    test_estimates_and_unpriced_records_mark_spend();
    test_inherited_items_are_context_not_bill();
    T_REPORT();
}
