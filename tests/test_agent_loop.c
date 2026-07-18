/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "agent_loop.h"
#include "agent_tool.h"
#include "config.h"
#include "harness.h"
#include "util.h"

enum script_mode {
    SCRIPT_COMPLETE,
    SCRIPT_PARTIAL_ERROR,
    SCRIPT_PRESTREAM_ERROR,
    SCRIPT_TOOL_ERROR,
};

static enum script_mode script;
static int observer_events;

static int emit_event(stream_cb cb, void *user, struct stream_event ev)
{
    return cb(&ev, user);
}

static int scripted_stream(struct provider *p, const struct context *ctx, const char *model,
                           stream_cb cb, void *user, http_tick_cb tick, void *tick_user)
{
    (void)p;
    (void)model;
    (void)tick;
    (void)tick_user;
    EXPECT(ctx != NULL);

    if (script == SCRIPT_PRESTREAM_ERROR) {
        emit_event(cb, user,
                   (struct stream_event){.kind = EV_ERROR,
                                         .u.error = {.message = "failed before output"}});
        return 0;
    }

    emit_event(cb, user,
               (struct stream_event){
                   .kind = EV_TEXT_DELTA,
                   .u.text_delta = {.text = script == SCRIPT_TOOL_ERROR ? "before" : "partial"}});
    if (script == SCRIPT_TOOL_ERROR) {
        emit_event(cb, user,
                   (struct stream_event){.kind = EV_TOOL_CALL_START,
                                         .u.tool_call_start = {.id = "c1", .name = "read"}});
        emit_event(cb, user,
                   (struct stream_event){.kind = EV_TOOL_CALL_DELTA,
                                         .u.tool_call_delta = {.id = "c1", .args_delta = "{}"}});
        emit_event(
            cb, user,
            (struct stream_event){.kind = EV_TOOL_CALL_END, .u.tool_call_end = {.id = "c1"}});
    }

    if (script == SCRIPT_COMPLETE) {
        struct stream_usage usage = {.input_tokens = 100,
                                     .output_tokens = 20,
                                     .cached_tokens = -1,
                                     .cache_write_tokens = -1,
                                     .cache_write_1h_tokens = -1,
                                     .cost = -1};
        emit_event(cb, user, (struct stream_event){.kind = EV_DONE, .u.done = {.usage = usage}});
    } else {
        static const struct stream_usage usage = {.input_tokens = 100,
                                                  .output_tokens = 20,
                                                  .cached_tokens = -1,
                                                  .cache_write_tokens = -1,
                                                  .cache_write_1h_tokens = -1,
                                                  .cost = -1};
        emit_event(cb, user,
                   (struct stream_event){.kind = EV_ERROR,
                                         .u.error = {.message = "stream failed", .usage = &usage}});
    }
    return 0;
}

static int observe(const struct stream_event *ev, void *user)
{
    (void)ev;
    (void)user;
    observer_events++;
    return 0;
}

static void session_init(struct agent_session *session)
{
    memset(session, 0, sizeof(*session));
    session->model = xstrdup("model");
}

static void test_loop_turn_collects_success(void)
{
    struct agent_session session;
    session_init(&session);
    struct provider provider = {.name = "test", .stream = scripted_stream};
    struct agent_loop_turn loop_turn;
    script = SCRIPT_COMPLETE;
    observer_events = 0;

    agent_loop_turn_run(&loop_turn, &session, &provider, observe, NULL, NULL, NULL);
    /* Collection is authoritative even with a presentation observer: terminal
     * usage, timing, and assembled text cannot depend on frontend behavior. */
    EXPECT(!loop_turn.assembly.error);
    EXPECT(loop_turn.usage.input_tokens == 100);
    EXPECT(loop_turn.usage.output_tokens == 20);
    EXPECT(loop_turn.elapsed_ms >= 0);
    EXPECT(observer_events == 2);

    size_t before;
    int had_tool_call;
    agent_session_absorb(&session, &loop_turn.assembly, &before, &had_tool_call);
    EXPECT(before == 0);
    EXPECT(!had_tool_call);
    EXPECT(session.n_items == 1);
    EXPECT_STR_EQ(session.items[0].text, "partial");

    agent_loop_turn_destroy(&loop_turn);
    agent_session_free(&session);
}

static void test_partial_error_is_preserved(void)
{
    struct agent_session session;
    session_init(&session);
    struct provider provider = {.name = "test", .stream = scripted_stream};
    struct agent_loop_turn loop_turn;
    script = SCRIPT_PARTIAL_ERROR;

    agent_loop_turn_run(&loop_turn, &session, &provider, NULL, NULL, NULL, NULL);
    EXPECT(loop_turn.assembly.error);
    EXPECT_STR_EQ(loop_turn.error_message, "stream failed");
    EXPECT(loop_turn.usage.input_tokens == 100);

    struct agent_abort_outcome out =
        agent_loop_turn_absorb_abort(&session, &loop_turn, AGENT_ABORT_PROVIDER_ERROR);
    /* Partial text is useful resumable state, but must be marked incomplete so
     * the next request does not treat it as a finished answer. */
    EXPECT(out.had_state);
    EXPECT(out.marker_placed);
    EXPECT(out.items_from == 0 && out.items_to == 1);
    EXPECT(session.n_items == 1);
    EXPECT_STR_EQ(session.items[0].text, "partial\n" INTERRUPT_MARKER);

    agent_loop_turn_destroy(&loop_turn);
    agent_session_free(&session);
}

static void test_prestream_error_adds_no_marker(void)
{
    struct agent_session session;
    session_init(&session);
    struct provider provider = {.name = "test", .stream = scripted_stream};
    struct agent_loop_turn loop_turn;
    script = SCRIPT_PRESTREAM_ERROR;

    agent_loop_turn_run(&loop_turn, &session, &provider, NULL, NULL, NULL, NULL);
    struct agent_abort_outcome out =
        agent_loop_turn_absorb_abort(&session, &loop_turn, AGENT_ABORT_PROVIDER_ERROR);
    /* A provider failure before any event must not fabricate assistant history;
     * retrying should see only the original prompt. */
    EXPECT(!out.had_state);
    EXPECT(!out.marker_placed);
    EXPECT(session.n_items == 0);

    agent_loop_turn_destroy(&loop_turn);
    agent_session_free(&session);
}

static void test_aborted_tool_call_gets_result(void)
{
    struct agent_session session;
    session_init(&session);
    struct provider provider = {.name = "test", .stream = scripted_stream};
    struct agent_loop_turn loop_turn;
    script = SCRIPT_TOOL_ERROR;

    agent_loop_turn_run(&loop_turn, &session, &provider, NULL, NULL, NULL, NULL);
    struct agent_abort_outcome out =
        agent_loop_turn_absorb_abort(&session, &loop_turn, AGENT_ABORT_PROVIDER_ERROR);
    /* Providers require call/result pairing on the next request, even though
     * this call was assembled immediately before the stream failed. */
    EXPECT(out.items_from == 0 && out.items_to == 2);
    EXPECT(session.n_items == 3);
    EXPECT(session.items[0].kind == ITEM_ASSISTANT_MESSAGE);
    EXPECT(session.items[1].kind == ITEM_TOOL_CALL);
    EXPECT(session.items[2].kind == ITEM_TOOL_RESULT);
    EXPECT_STR_EQ(session.items[2].call_id, "c1");
    EXPECT_STR_EQ(session.items[2].output, INTERRUPT_MARKER);

    agent_loop_turn_destroy(&loop_turn);
    agent_session_free(&session);
}

static void test_empty_cancel_adds_marker(void)
{
    struct agent_session session;
    session_init(&session);
    struct agent_loop_turn loop_turn = {0};
    turn_init(&loop_turn.assembly);
    loop_turn.usage = (struct stream_usage){-1, -1, -1, -1, -1, -1};

    struct agent_abort_outcome out =
        agent_loop_turn_absorb_abort(&session, &loop_turn, AGENT_ABORT_USER_CANCEL);
    /* Unlike a pre-stream provider failure, an explicit user stop belongs in
     * history even when no model bytes arrived. */
    EXPECT(!out.had_state);
    EXPECT(out.marker_placed);
    EXPECT(session.n_items == 1);
    EXPECT_STR_EQ(session.items[0].text, INTERRUPT_MARKER);

    agent_loop_turn_destroy(&loop_turn);
    agent_session_free(&session);
}

static int chain_turn;
static int chain_two_tools;

static void emit_tool_call(stream_cb cb, void *user, const char *id)
{
    emit_event(cb, user,
               (struct stream_event){.kind = EV_TOOL_CALL_START,
                                     .u.tool_call_start = {.id = id, .name = "read"}});
    emit_event(cb, user,
               (struct stream_event){.kind = EV_TOOL_CALL_DELTA,
                                     .u.tool_call_delta = {.id = id, .args_delta = "{}"}});
    emit_event(cb, user,
               (struct stream_event){.kind = EV_TOOL_CALL_END, .u.tool_call_end = {.id = id}});
}

static int chain_stream(struct provider *p, const struct context *ctx, const char *model,
                        stream_cb cb, void *user, http_tick_cb tick, void *tick_user)
{
    (void)p;
    (void)model;
    (void)tick;
    (void)tick_user;
    EXPECT(ctx != NULL);
    chain_turn++;

    if (chain_turn == 1) {
        emit_tool_call(cb, user, "c1");
        if (chain_two_tools)
            emit_tool_call(cb, user, "c2");
    } else {
        emit_event(
            cb, user,
            (struct stream_event){.kind = EV_TEXT_DELTA, .u.text_delta = {.text = "finished"}});
    }
    struct stream_usage usage = {.input_tokens = 10 * chain_turn,
                                 .output_tokens = 2,
                                 .cached_tokens = -1,
                                 .cache_write_tokens = -1,
                                 .cache_write_1h_tokens = -1,
                                 .cost = -1};
    emit_event(cb, user, (struct stream_event){.kind = EV_DONE, .u.done = {.usage = usage}});
    return 0;
}

struct loop_test_ctx {
    int tools_seen;
    int tools_run;
    int tools_skipped;
    int turns;
    int compactions;
    int checkpoints;
    int cancel_at;
};

static void count_turn(const struct agent_loop_turn *loop_turn, void *user)
{
    struct loop_test_ctx *ctx = user;
    EXPECT(loop_turn->elapsed_ms >= 0);
    ctx->turns++;
}

static void count_tool(const struct item *call, void *user)
{
    struct loop_test_ctx *ctx = user;
    EXPECT(call->kind == ITEM_TOOL_CALL);
    ctx->tools_seen++;
}

static struct item run_test_tool(const struct item *call, enum agent_loop_tool_action action,
                                 void *user)
{
    struct loop_test_ctx *ctx = user;
    if (action == AGENT_LOOP_TOOL_RUN) {
        ctx->tools_run++;
        return agent_tool_result_make(call, "handled");
    }
    if (action == AGENT_LOOP_TOOL_SKIP) {
        ctx->tools_skipped++;
        return agent_tool_result_make(call, INTERRUPT_MARKER);
    }
    return agent_tool_result_make(call, "refused");
}

static int cancel_checkpoint(void *user)
{
    struct loop_test_ctx *ctx = user;
    ctx->checkpoints++;
    return ctx->cancel_at > 0 && ctx->checkpoints >= ctx->cancel_at;
}

static void count_compaction(void *user)
{
    struct loop_test_ctx *ctx = user;
    ctx->compactions++;
}

static void session_enable_tools(struct agent_session *session)
{
    session->tools = xcalloc(1, sizeof(*session->tools));
    session->tools[0].name = "read";
    session->n_tools = 1;
}

static struct agent_loop_result run_chain(struct agent_session *session, struct provider *provider,
                                          struct loop_test_ctx *ctx, int max_turns)
{
    struct agent_loop_params params = {
        .session = session,
        .provider = provider,
        .max_turns = max_turns,
        .hooks =
            {
                .user = ctx,
                .turn_end = count_turn,
                .checkpoint = ctx->cancel_at ? cancel_checkpoint : NULL,
                .tool_seen = count_tool,
                .tool_call = run_test_tool,
                .compact = count_compaction,
            },
    };
    struct agent_loop_result result;
    agent_loop_run(&params, &result);
    return result;
}

static void test_loop_runs_tool_chain(void)
{
    struct agent_session session;
    session_init(&session);
    session_enable_tools(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = chain_stream};
    struct loop_test_ctx ctx = {0};
    chain_turn = 0;
    chain_two_tools = 0;

    struct agent_loop_result result = run_chain(&session, &provider, &ctx, 4);
    /* One tool turn must continue into one text-only turn, with accounting
     * hooks firing once per provider request and context reflecting the latest
     * request rather than a sum. */
    EXPECT(result.outcome == AGENT_LOOP_COMPLETE);
    EXPECT(result.turns == 2 && ctx.turns == 2);
    EXPECT(result.last_context_tokens == 22);
    EXPECT(ctx.tools_seen == 1 && ctx.tools_run == 1);
    /* The returned range is presentation-safe: final streamed text only, not
     * its usage footer or any prior call/result items. */
    EXPECT(result.final_items_to == result.final_items_from + 1);
    EXPECT_STR_EQ(session.items[result.final_items_from].text, "finished");

    int boundaries = 0;
    int tool_results = 0;
    int usage_items = 0;
    for (size_t i = 0; i < session.n_items; i++) {
        boundaries += session.items[i].kind == ITEM_TURN_BOUNDARY;
        tool_results += session.items[i].kind == ITEM_TOOL_RESULT;
        usage_items += session.items[i].kind == ITEM_TURN_USAGE;
    }
    /* Transcript structure stays one boundary and one footer per provider
     * turn, with a matching result for every call. */
    EXPECT(boundaries == 2);
    EXPECT(tool_results == 1);
    EXPECT(usage_items == 2);

    agent_loop_result_destroy(&result);
    agent_session_free(&session);
}

static void test_loop_enforces_max_turns(void)
{
    struct agent_session session;
    session_init(&session);
    session_enable_tools(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = chain_stream};
    struct loop_test_ctx ctx = {0};
    chain_turn = 0;
    chain_two_tools = 0;

    struct agent_loop_result result = run_chain(&session, &provider, &ctx, 1);
    /* The cap prevents the follow-up provider request, but the accepted turn's
     * tool call still needs execution and a matching result in history. */
    EXPECT(result.outcome == AGENT_LOOP_MAX_TURNS);
    EXPECT(result.turns == 1 && chain_turn == 1);
    EXPECT(ctx.tools_run == 1);

    agent_loop_result_destroy(&result);
    agent_session_free(&session);
}

static void test_loop_cancels_tool_batch(void)
{
    struct agent_session session;
    session_init(&session);
    session_enable_tools(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = chain_stream};
    /* Checkpoints: after stream, before c1, before c2. */
    struct loop_test_ctx ctx = {.cancel_at = 3};
    chain_turn = 0;
    chain_two_tools = 1;

    struct agent_loop_result result = run_chain(&session, &provider, &ctx, 4);
    /* Cancellation between calls runs the completed prefix, skips the suffix,
     * and still pairs the whole batch so resumed history is valid. */
    EXPECT(result.outcome == AGENT_LOOP_INTERRUPTED);
    EXPECT(ctx.tools_seen == 2);
    EXPECT(ctx.tools_run == 1);
    EXPECT(ctx.tools_skipped == 1);
    /* The interrupted result carries the stop marker; the footer remains last
     * so it continues to describe the complete provider turn. */
    EXPECT_STR_EQ(session.items[session.n_items - 2].output, INTERRUPT_MARKER);
    EXPECT(session.items[session.n_items - 1].kind == ITEM_TURN_USAGE);

    agent_loop_result_destroy(&result);
    agent_session_free(&session);
}

static void test_loop_reports_provider_error(void)
{
    struct agent_session session;
    session_init(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = scripted_stream};
    script = SCRIPT_PARTIAL_ERROR;
    struct agent_loop_params params = {
        .session = &session,
        .provider = &provider,
        .max_turns = 2,
    };

    struct agent_loop_result result;
    agent_loop_run(&params, &result);
    /* A billed partial failure is resumable: preserve and mark its text, expose
     * the diagnostic, and retain the usage footer without continuing. */
    EXPECT(result.outcome == AGENT_LOOP_PROVIDER_ERROR);
    EXPECT_STR_EQ(result.error_message, "stream failed");
    EXPECT(result.turns == 1);
    EXPECT_STR_EQ(session.items[result.final_items_from].text, "partial\n" INTERRUPT_MARKER);
    EXPECT(session.items[session.n_items - 1].kind == ITEM_TURN_USAGE);

    agent_loop_result_destroy(&result);
    agent_session_free(&session);
}

static void test_loop_requests_mid_chain_compaction(void)
{
    config_set_override("compact.auto", "1");
    config_set_override("compact.threshold", "50");
    config_set_override("context_limit", "10");

    struct agent_session session;
    session_init(&session);
    session_enable_tools(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = chain_stream};
    struct loop_test_ctx ctx = {0};
    chain_turn = 0;
    chain_two_tools = 0;

    struct agent_loop_result result = run_chain(&session, &provider, &ctx, 4);
    /* Crossing the threshold requests compaction only at the tool-continuation
     * seam; the final text-only turn must not trigger another transaction. */
    EXPECT(result.outcome == AGENT_LOOP_COMPLETE);
    EXPECT(ctx.compactions == 1);

    agent_loop_result_destroy(&result);
    agent_session_free(&session);
    config_set_override("context_limit", NULL);
    config_set_override("compact.threshold", NULL);
    config_set_override("compact.auto", NULL);
}

int main(void)
{
    test_loop_turn_collects_success();
    test_partial_error_is_preserved();
    test_prestream_error_adds_no_marker();
    test_aborted_tool_call_gets_result();
    test_empty_cancel_adds_marker();
    test_loop_runs_tool_chain();
    test_loop_enforces_max_turns();
    test_loop_cancels_tool_batch();
    test_loop_reports_provider_error();
    test_loop_requests_mid_chain_compaction();
    T_REPORT();
}
