/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "agent_core.h"
#include "harness.h"
#include "output.h"
#include "provider.h"
#include "xalloc.h"
#include "commands/session_cmd.h"

static void print_report(void *state)
{
    session_command(state);
}

/* One user turn: prompt, reply, and a footer whose costs are given directly, as a provider
 * that reports charges would leave them. */
static void add_priced_user_turn(struct agent_session *session, const char *provider,
                                 const char *model, long input, long output, long cached,
                                 long cache_write, double cost, int estimated)
{
    agent_session_add_user(session, "prompt");
    agent_session_append(session,
                         (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup("reply")});
    struct turn_usage *usage = xcalloc(1, sizeof(*usage));
    usage->usage =
        (struct stream_usage){input, output, cached, cache_write, -1, estimated ? -1 : cost};
    usage->elapsed_ms = 1000;
    usage->uncached_input_tokens =
        input - (cached > 0 ? cached : 0) - (cache_write > 0 ? cache_write : 0);
    usage->cost_input = -1;
    usage->cost_cache_read = -1;
    usage->cost_cache_write = -1;
    usage->cost_output = -1;
    usage->cost_total = cost;
    usage->cost_estimated = estimated;
    agent_session_append(session, (struct item){.kind = ITEM_TURN_USAGE,
                                                .usage = usage,
                                                .provider = xstrdup(provider),
                                                .model = xstrdup(model)});
}

static void add_tool_call(struct agent_session *session, const char *tool_name)
{
    agent_session_append(session, (struct item){.kind = ITEM_TOOL_CALL,
                                                .call_id = xstrdup("c"),
                                                .tool_name = xstrdup(tool_name),
                                                .tool_arguments_json = xstrdup("{}")});
}

static void test_session_prints_totals(void)
{
    struct agent_session s = {0};
    add_priced_user_turn(&s, "prov", "m", 2000, 200, 1024, 512, 0.02, 0);
    add_tool_call(&s, "bash");
    add_tool_call(&s, "bash");
    add_tool_call(&s, "read");
    add_priced_user_turn(&s, "prov", "m", 3530, 212, 1024, 512, 0.022, 0);
    agent_session_add_worked(&s, 68000);
    struct agent_state state = {.session = &s};
    char *out = t_capture_stdout(print_report, &state);
    EXPECT(strstr(out, "not recorded") != NULL);
    EXPECT(strstr(out, "user turns") != NULL);
    EXPECT(strstr(out, "requests") != NULL);
    EXPECT(strstr(out, "tool calls") != NULL);
    EXPECT(strstr(out, "3 · bash 2 · read 1") != NULL);
    EXPECT(strstr(out, "time worked") != NULL);
    EXPECT(strstr(out, "1m 08s") != NULL);
    EXPECT(strstr(out, "context") != NULL);
    EXPECT(strstr(out, "3.7k") != NULL);
    EXPECT(strstr(out, "tokens") != NULL);
    EXPECT(strstr(out, "in 2.5k · cache 2k · write 1k · out 412") != NULL);
    EXPECT(strstr(out, "$0.042") != NULL);
    EXPECT(strstr(out, "~$") == NULL);
    EXPECT(strstr(out, "undone") == NULL);
    free(out);
    agent_session_free(&s);
}

static void test_session_hides_unreported_rows(void)
{
    struct agent_state state = {0};
    char *out = t_capture_stdout(print_report, &state);
    /* Identity rows stay; zero-activity and unknown measurements are omitted. */
    EXPECT(strstr(out, "not recorded") != NULL);
    EXPECT(strstr(out, "provider") != NULL);
    EXPECT(strstr(out, "user turns") == NULL);
    EXPECT(strstr(out, "requests") == NULL);
    EXPECT(strstr(out, "time worked") == NULL);
    EXPECT(strstr(out, "tool calls") == NULL);
    EXPECT(strstr(out, "context") == NULL);
    EXPECT(strstr(out, "tokens") == NULL);
    EXPECT(strstr(out, "$") == NULL);
    free(out);
}

static void test_session_shows_window_before_first_request(void)
{
    struct agent_state state = {0};

    setenv("HAX_CONTEXT_LIMIT", "262144", 1);
    char *out = t_capture_stdout(print_report, &state);
    unsetenv("HAX_CONTEXT_LIMIT");
    EXPECT(strstr(out, "context") != NULL);
    EXPECT(strstr(out, "? / 262k") != NULL);
    EXPECT(strstr(out, "%") == NULL);
    free(out);
}

static void test_session_marks_estimated_spend(void)
{
    struct agent_session s = {0};
    add_priced_user_turn(&s, "prov", "m", 1000, 50, -1, -1, 0.010, 0);
    add_priced_user_turn(&s, "prov", "m", 1000, 50, -1, -1, 0.020, 1);
    struct agent_state state = {.session = &s};
    char *out = t_capture_stdout(print_report, &state);
    EXPECT(strstr(out, "~$0.030") != NULL);
    free(out);
    agent_session_free(&s);
}

/* A model switch mid-conversation gets one token row per model, and undone user turns stay in
 * the totals, flagged on the count the screen no longer shows. */
static void test_session_splits_models_and_counts_undone(void)
{
    struct agent_session s = {0};
    add_priced_user_turn(&s, "prov", "small", 1000, 100, -1, -1, 0.01, 0);
    add_priced_user_turn(&s, "prov", "large", 2000, 200, -1, -1, 0.10, 0);
    add_priced_user_turn(&s, "prov", "large", 3000, 300, -1, -1, 0.20, 0);
    agent_session_retire(&s, items_user_turn_cut(s.items, s.n_items, 2));
    struct agent_state state = {.session = &s};
    char *out = t_capture_stdout(print_report, &state);
    EXPECT(strstr(out, "prov · small") != NULL);
    EXPECT(strstr(out, "$0.010 · in 1k · out 100") != NULL);
    EXPECT(strstr(out, "prov · large") != NULL);
    EXPECT(strstr(out, "$0.300 · in 5k · out 500") != NULL);
    EXPECT(strstr(out, "2 · 1 undone") != NULL);
    EXPECT(strstr(out, "$0.31") != NULL);
    free(out);
    agent_session_free(&s);
}

static void test_session_wraps_to_narrow_width(void)
{
    struct agent_session s = {0};
    add_priced_user_turn(&s, "prov", "m", 5530, 412, 2048, 1024, -1, 1);
    struct agent_state state = {.session = &s};

    setenv("HAX_DISPLAY_WIDTH", "30", 1);
    char *raw = t_capture_stdout(print_report, &state);
    unsetenv("HAX_DISPLAY_WIDTH");

    char *out = t_strip_sgr(raw);
    free(raw);
    t_expect_rows_fit(out, 30);
    /* The token row wraps at segment spaces rather than truncating. */
    EXPECT(strstr(out, "tokens") != NULL);
    EXPECT(strstr(out, "out 412") != NULL);
    free(out);
    agent_session_free(&s);
}

int main(void)
{
    /* Row-layout and row-presence assertions depend on these; the variables leak in from any
     * hax parent or user environment. */
    unsetenv("HAX_DISPLAY_WIDTH");
    unsetenv("HAX_CONTEXT_LIMIT");

    test_session_prints_totals();
    test_session_hides_unreported_rows();
    test_session_shows_window_before_first_request();
    test_session_marks_estimated_spend();
    test_session_splits_models_and_counts_undone();
    test_session_wraps_to_narrow_width();
    T_REPORT();
}
