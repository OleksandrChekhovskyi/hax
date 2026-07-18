/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "agent_loop.h"
#include "harness.h"
#include "tool.h"
#include "util.h"

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
    EXPECT(!out.had_state);
    EXPECT(out.marker_placed);
    EXPECT(session.n_items == 1);
    EXPECT_STR_EQ(session.items[0].text, INTERRUPT_MARKER);

    agent_loop_turn_destroy(&loop_turn);
    agent_session_free(&session);
}

int main(void)
{
    test_loop_turn_collects_success();
    test_partial_error_is_preserved();
    test_prestream_error_adds_no_marker();
    test_aborted_tool_call_gets_result();
    test_empty_cancel_adds_marker();
    T_REPORT();
}
