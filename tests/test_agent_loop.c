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
    int tools_refused;
    int turn_begins;
    int turns;
    int compactions;
    int checkpoints;
    int cancel_at;
    int pause_at;
    int continued;
};

static void count_turn_begin(void *user)
{
    struct loop_test_ctx *ctx = user;
    ctx->turn_begins++;
}

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
    ctx->tools_refused++;
    return agent_tool_result_make(call, "refused");
}

static int cancel_checkpoint(void *user)
{
    struct loop_test_ctx *ctx = user;
    ctx->checkpoints++;
    if (ctx->cancel_at > 0 && ctx->checkpoints >= ctx->cancel_at)
        return AGENT_LOOP_SIG_ABORT;
    if (ctx->pause_at > 0 && ctx->checkpoints >= ctx->pause_at)
        return AGENT_LOOP_SIG_PAUSE;
    return AGENT_LOOP_SIG_NONE;
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
        .continued = ctx->continued,
        .hooks =
            {
                .user = ctx,
                .turn_begin = count_turn_begin,
                .turn_end = count_turn,
                .checkpoint = (ctx->cancel_at || ctx->pause_at) ? cancel_checkpoint : NULL,
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
    /* One tool turn must continue into one text-only turn, with frontend
     * begin and accounting hooks firing once per turn; context reflects the
     * latest turn rather than a sum. */
    EXPECT(result.outcome == AGENT_LOOP_COMPLETE);
    EXPECT(result.turns == 2 && ctx.turn_begins == 2 && ctx.turns == 2);
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
    EXPECT(result.abort_marker_placed);
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

static void test_loop_cancel_still_refuses_disabled_tool(void)
{
    struct agent_session session;
    session_init(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = chain_stream};
    /* Cancellation is first observed immediately before c1. */
    struct loop_test_ctx ctx = {.cancel_at = 2};
    chain_turn = 0;
    chain_two_tools = 0;

    struct agent_loop_result result = run_chain(&session, &provider, &ctx, 4);
    /* The checkpoint must still run, but disabled-tool refusal takes precedence:
     * a malformed backend call is never represented as an ordinary skipped
     * tool that might hide the raw-mode policy violation. */
    EXPECT(result.outcome == AGENT_LOOP_INTERRUPTED);
    EXPECT(ctx.tools_run == 0 && ctx.tools_skipped == 0);
    EXPECT(ctx.tools_refused == 1);

    agent_loop_result_destroy(&result);
    agent_session_free(&session);
}

static void test_loop_pause_runs_whole_batch(void)
{
    struct agent_session session;
    session_init(&session);
    session_enable_tools(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = chain_stream};
    /* Pause is first observed at the post-stream checkpoint, before any
     * tool has launched. */
    struct loop_test_ctx ctx = {.pause_at = 1};
    chain_turn = 0;
    chain_two_tools = 1;

    struct agent_loop_result result = run_chain(&session, &provider, &ctx, 4);
    /* A pause must not cancel in-flight work: the whole accepted batch still
     * runs and the stop lands at the seam, before the follow-up request. */
    EXPECT(result.outcome == AGENT_LOOP_PAUSED);
    EXPECT(chain_turn == 1);
    EXPECT(ctx.tools_run == 2 && ctx.tools_skipped == 0);
    EXPECT(!result.abort_marker_placed);

    /* The seam is clean: every call paired, no interrupt markers anywhere,
     * usage footer trailing — history a later run can continue verbatim. */
    int calls = 0;
    int results = 0;
    for (size_t i = 0; i < session.n_items; i++) {
        calls += session.items[i].kind == ITEM_TOOL_CALL;
        results += session.items[i].kind == ITEM_TOOL_RESULT;
        if (session.items[i].kind == ITEM_ASSISTANT_MESSAGE)
            EXPECT(strcmp(session.items[i].text, INTERRUPT_MARKER) != 0);
        if (session.items[i].kind == ITEM_TOOL_RESULT)
            EXPECT_STR_EQ(session.items[i].output, "handled");
    }
    EXPECT(calls == 2 && results == 2);
    EXPECT(session.items[session.n_items - 1].kind == ITEM_TURN_USAGE);

    /* Resuming the paused history runs the follow-up turn to completion. */
    agent_session_add_boundary(&session);
    struct loop_test_ctx resume_ctx = {0};
    struct agent_loop_result resumed = run_chain(&session, &provider, &resume_ctx, 4);
    EXPECT(resumed.outcome == AGENT_LOOP_COMPLETE);
    EXPECT_STR_EQ(session.items[resumed.final_items_from].text, "finished");

    agent_loop_result_destroy(&resumed);
    agent_loop_result_destroy(&result);
    agent_session_free(&session);
}

/* A stream that ends without emitting any event — the shape a soft
 * interrupt leaves when the frontend tick cancels a request that had
 * produced no content yet (still prefilling). */
static int silent_stream(struct provider *p, const struct context *ctx, const char *model,
                         stream_cb cb, void *user, http_tick_cb tick, void *tick_user)
{
    (void)p;
    (void)model;
    (void)cb;
    (void)user;
    (void)tick;
    (void)tick_user;
    EXPECT(ctx != NULL);
    return 0;
}

static void test_loop_pause_preempts_empty_turn(void)
{
    struct agent_session session;
    session_init(&session);
    session_enable_tools(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = silent_stream};
    struct loop_test_ctx ctx = {.pause_at = 1};

    struct agent_loop_result result = run_chain(&session, &provider, &ctx, 4);
    /* An empty turn under a pause request is a pre-empted request, not a
     * completion: pause with history untouched so a resume re-sends it. */
    EXPECT(result.outcome == AGENT_LOOP_PAUSED);
    EXPECT(!result.abort_marker_placed);
    EXPECT(session.n_items == 2); /* boundary + user message only */

    agent_loop_result_destroy(&result);
    agent_session_free(&session);
}

/* Turn 1 emits a tool call and completes; turn 2 emits nothing — the shape
 * of a follow-up request pre-empted by a soft interrupt during prefill. */
static int chain_then_silent_stream(struct provider *p, const struct context *ctx,
                                    const char *model, stream_cb cb, void *user, http_tick_cb tick,
                                    void *tick_user)
{
    (void)p;
    (void)model;
    (void)tick;
    (void)tick_user;
    EXPECT(ctx != NULL);
    chain_turn++;
    if (chain_turn > 1)
        return 0;
    emit_tool_call(cb, user, "c1");
    struct stream_usage usage = {.input_tokens = 10,
                                 .output_tokens = 2,
                                 .cached_tokens = -1,
                                 .cache_write_tokens = -1,
                                 .cache_write_1h_tokens = -1,
                                 .cost = -1};
    emit_event(cb, user, (struct stream_event){.kind = EV_DONE, .u.done = {.usage = usage}});
    return 0;
}

static void test_loop_pause_preempts_follow_up_leaves_no_boundary(void)
{
    struct agent_session session;
    session_init(&session);
    session_enable_tools(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = chain_then_silent_stream};
    /* Turn 1 checkpoints: post-stream, pre-c1, final. The pause is first
     * observed at turn 2's post-stream checkpoint. */
    struct loop_test_ctx ctx = {.pause_at = 4};
    chain_turn = 0;

    struct agent_loop_result result = run_chain(&session, &provider, &ctx, 4);
    /* The pre-empted follow-up turn must leave no trace: no dangling
     * boundary (which the transcript would render as an empty turn), no
     * marker, no footer — history still ends at turn 1's seam. */
    EXPECT(result.outcome == AGENT_LOOP_PAUSED);
    EXPECT(session.items[session.n_items - 1].kind == ITEM_TURN_USAGE);

    int boundaries = 0;
    for (size_t i = 0; i < session.n_items; i++)
        boundaries += session.items[i].kind == ITEM_TURN_BOUNDARY;
    EXPECT(boundaries == 1); /* only the user message's */

    agent_loop_result_destroy(&result);
    agent_session_free(&session);
}

static void test_loop_continued_run_owes_boundary(void)
{
    /* A continued run (empty-send resume): the first turn extends the prior
     * seam, so once it leaves items it owes a boundary like a follow-up. */
    struct agent_session session;
    session_init(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = scripted_stream};
    script = SCRIPT_COMPLETE;
    struct loop_test_ctx ctx = {.continued = 1};

    struct agent_loop_result result = run_chain(&session, &provider, &ctx, 4);
    EXPECT(result.outcome == AGENT_LOOP_COMPLETE);
    int boundaries = 0;
    for (size_t i = 0; i < session.n_items; i++)
        boundaries += session.items[i].kind == ITEM_TURN_BOUNDARY;
    EXPECT(boundaries == 2); /* the user message's, plus the continued turn's */

    agent_loop_result_destroy(&result);
    agent_session_free(&session);
}

static void test_loop_continued_preempted_run_leaves_no_boundary(void)
{
    /* A continued run pre-empted again during prefill must stay traceless —
     * an eagerly appended boundary here is exactly the dangling empty turn
     * the lazy scheme exists to avoid. */
    struct agent_session session;
    session_init(&session);
    session_enable_tools(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = silent_stream};
    struct loop_test_ctx ctx = {.pause_at = 1, .continued = 1};

    struct agent_loop_result result = run_chain(&session, &provider, &ctx, 4);
    EXPECT(result.outcome == AGENT_LOOP_PAUSED);
    EXPECT(session.n_items == 2); /* boundary + user message only */

    agent_loop_result_destroy(&result);
    agent_session_free(&session);
}

/* Turn 1 emits a tool call; turn 2 completes successfully with no content
 * (EV_DONE only, usage reported) — an empty but real round-trip. */
static int chain_then_empty_done_stream(struct provider *p, const struct context *ctx,
                                        const char *model, stream_cb cb, void *user,
                                        http_tick_cb tick, void *tick_user)
{
    (void)p;
    (void)model;
    (void)tick;
    (void)tick_user;
    EXPECT(ctx != NULL);
    chain_turn++;
    if (chain_turn == 1)
        emit_tool_call(cb, user, "c1");
    struct stream_usage usage = {.input_tokens = 10 * chain_turn,
                                 .output_tokens = 2,
                                 .cached_tokens = -1,
                                 .cache_write_tokens = -1,
                                 .cache_write_1h_tokens = -1,
                                 .cost = -1};
    emit_event(cb, user, (struct stream_event){.kind = EV_DONE, .u.done = {.usage = usage}});
    return 0;
}

static void test_loop_empty_completion_keeps_boundary(void)
{
    struct agent_session session;
    session_init(&session);
    session_enable_tools(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = chain_then_empty_done_stream};
    struct loop_test_ctx ctx = {0};
    chain_turn = 0;

    struct agent_loop_result result = run_chain(&session, &provider, &ctx, 4);
    /* An empty but *completed* follow-up is a real round-trip: it owes its
     * own boundary so its usage footer isn't attributed to the preceding
     * tool turn — only a pause-cancelled stream (no EV_DONE) leaves no
     * trace. */
    EXPECT(result.outcome == AGENT_LOOP_COMPLETE);
    EXPECT(result.turns == 2);

    int boundaries = 0;
    int usage_items = 0;
    for (size_t i = 0; i < session.n_items; i++) {
        boundaries += session.items[i].kind == ITEM_TURN_BOUNDARY;
        usage_items += session.items[i].kind == ITEM_TURN_USAGE;
    }
    EXPECT(boundaries == 2);
    EXPECT(usage_items == 2);
    /* The empty turn's representation is exactly boundary + footer. */
    EXPECT(session.items[session.n_items - 2].kind == ITEM_TURN_BOUNDARY);
    EXPECT(session.items[session.n_items - 1].kind == ITEM_TURN_USAGE);

    agent_loop_result_destroy(&result);
    agent_session_free(&session);
}

static void test_loop_pause_on_final_turn_completes(void)
{
    struct agent_session session;
    session_init(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = scripted_stream};
    script = SCRIPT_COMPLETE;
    struct loop_test_ctx ctx = {.pause_at = 1};

    struct agent_loop_result result = run_chain(&session, &provider, &ctx, 4);
    /* A pause requested during a tool-free response is moot: the user turn
     * finishes right here and there is nothing left to resume. */
    EXPECT(result.outcome == AGENT_LOOP_COMPLETE);

    agent_loop_result_destroy(&result);
    agent_session_free(&session);
}

static void test_loop_pause_defers_seam_compaction(void)
{
    config_set_override("compact.auto", "1");
    config_set_override("compact.threshold", "50");
    config_set_override("context_limit", "10");

    struct agent_session session;
    session_init(&session);
    session_enable_tools(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = chain_stream};
    /* Pause lands at the final seam checkpoint (post-stream, pre-c1, final). */
    struct loop_test_ctx ctx = {.pause_at = 3};
    chain_turn = 0;
    chain_two_tools = 0;

    struct agent_loop_result result = run_chain(&session, &provider, &ctx, 4);
    /* A pause returns control without launching new work, even at an
     * over-threshold seam: the owed compaction belongs to the run that
     * continues the turn (the frontend settles it before re-entry). */
    EXPECT(result.outcome == AGENT_LOOP_PAUSED);
    EXPECT(ctx.compactions == 0);

    agent_loop_result_destroy(&result);
    agent_session_free(&session);
    config_set_override("context_limit", NULL);
    config_set_override("compact.threshold", NULL);
    config_set_override("compact.auto", NULL);
}

/* Turn 1 emits a tool call and completes; turn 2 fails before any content
 * but reports billable usage on its EV_ERROR. */
static int chain_then_error_usage_stream(struct provider *p, const struct context *ctx,
                                         const char *model, stream_cb cb, void *user,
                                         http_tick_cb tick, void *tick_user)
{
    (void)p;
    (void)model;
    (void)tick;
    (void)tick_user;
    EXPECT(ctx != NULL);
    chain_turn++;
    if (chain_turn == 1) {
        emit_tool_call(cb, user, "c1");
        struct stream_usage usage = {.input_tokens = 10,
                                     .output_tokens = 2,
                                     .cached_tokens = -1,
                                     .cache_write_tokens = -1,
                                     .cache_write_1h_tokens = -1,
                                     .cost = -1};
        emit_event(cb, user, (struct stream_event){.kind = EV_DONE, .u.done = {.usage = usage}});
        return 0;
    }
    static const struct stream_usage usage = {.input_tokens = 20,
                                              .output_tokens = 0,
                                              .cached_tokens = -1,
                                              .cache_write_tokens = -1,
                                              .cache_write_1h_tokens = -1,
                                              .cost = -1};
    emit_event(
        cb, user,
        (struct stream_event){.kind = EV_ERROR, .u.error = {.message = "boom", .usage = &usage}});
    return 0;
}

static void test_loop_error_with_usage_keeps_boundary(void)
{
    struct agent_session session;
    session_init(&session);
    session_enable_tools(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = chain_then_error_usage_stream};
    struct loop_test_ctx ctx = {0};
    chain_turn = 0;

    struct agent_loop_result result = run_chain(&session, &provider, &ctx, 4);
    /* The failed follow-up produced no content but its reported usage is
     * retained as a footer — which owes the boundary, or the footer would
     * read as part of the preceding tool turn. */
    EXPECT(result.outcome == AGENT_LOOP_PROVIDER_ERROR);
    EXPECT(!result.abort_marker_placed);
    EXPECT(session.items[session.n_items - 1].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[session.n_items - 2].kind == ITEM_TURN_BOUNDARY);

    int boundaries = 0;
    int usage_items = 0;
    for (size_t i = 0; i < session.n_items; i++) {
        boundaries += session.items[i].kind == ITEM_TURN_BOUNDARY;
        usage_items += session.items[i].kind == ITEM_TURN_USAGE;
    }
    EXPECT(boundaries == 2);
    EXPECT(usage_items == 2);

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
    /* Repair marked the partial text, so a resume must speak for the user. */
    EXPECT(result.abort_marker_placed);

    agent_loop_result_destroy(&result);
    agent_session_free(&session);
}

static void test_loop_prestream_error_leaves_no_marker(void)
{
    struct agent_session session;
    session_init(&session);
    agent_session_add_user(&session, "start");
    struct provider provider = {.name = "test", .stream = scripted_stream};
    script = SCRIPT_PRESTREAM_ERROR;
    struct agent_loop_params params = {
        .session = &session,
        .provider = &provider,
        .max_turns = 2,
    };

    struct agent_loop_result result;
    agent_loop_run(&params, &result);
    /* A failure before any output leaves history untouched — a marker-free
     * stop, so an empty-send resume is a pure retry with nothing appended. */
    EXPECT(result.outcome == AGENT_LOOP_PROVIDER_ERROR);
    EXPECT(!result.abort_marker_placed);
    EXPECT(session.n_items == 2); /* boundary + user message only */

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
    /* Loop tests exercise orchestration, not the platform inhibitor helper. */
    config_set_override("keep_awake", "0");
    test_loop_turn_collects_success();
    test_partial_error_is_preserved();
    test_prestream_error_adds_no_marker();
    test_aborted_tool_call_gets_result();
    test_empty_cancel_adds_marker();
    test_loop_runs_tool_chain();
    test_loop_enforces_max_turns();
    test_loop_cancels_tool_batch();
    test_loop_cancel_still_refuses_disabled_tool();
    test_loop_pause_runs_whole_batch();
    test_loop_pause_preempts_empty_turn();
    test_loop_pause_preempts_follow_up_leaves_no_boundary();
    test_loop_continued_run_owes_boundary();
    test_loop_continued_preempted_run_leaves_no_boundary();
    test_loop_empty_completion_keeps_boundary();
    test_loop_pause_on_final_turn_completes();
    test_loop_pause_defers_seam_compaction();
    test_loop_error_with_usage_keeps_boundary();
    test_loop_reports_provider_error();
    test_loop_prestream_error_leaves_no_marker();
    test_loop_requests_mid_chain_compaction();
    T_REPORT();
}
