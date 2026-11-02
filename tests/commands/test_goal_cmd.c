/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "agent_core.h"
#include "goal.h"
#include "harness.h"
#include "output.h"
#include "provider.h"
#include "xalloc.h"
#include "commands/goal_cmd.h"

struct command_call {
    struct agent_state *state;
    const char *argument;
};

static void run_command(void *user)
{
    struct command_call *call = user;
    goal_command(call->state, call->argument);
}

static void test_goal_command(void)
{
    struct agent_session session = {0};
    struct agent_state state = {.session = &session};
    struct command_call call = {.state = &state};
    char *out = t_capture_stdout(run_command, &call);
    EXPECT(strstr(out, "no active goal") != NULL);
    free(out);

    call.argument = "fix all failing tests";
    out = t_capture_stdout(run_command, &call);
    free(out);
    EXPECT_STR_EQ(state.pending_prompt, call.argument);
    EXPECT(session.n_items == 0);
    free(state.pending_prompt);
    state.pending_prompt = NULL;

    agent_session_append(&session, (struct item){.kind = ITEM_USER_MESSAGE,
                                                 .text = xstrdup("fix tests"),
                                                 .origin = ITEM_ORIGIN_GOAL});
    call.argument = NULL;
    out = t_capture_stdout(run_command, &call);
    EXPECT(strstr(out, "goal: fix tests") != NULL);
    free(out);
    call.argument = "clear";
    out = t_capture_stdout(run_command, &call);
    EXPECT(strstr(out, "goal cleared") != NULL);
    free(out);
    EXPECT(goal_current(&session) == NULL);
    EXPECT(session.n_items == 2);
    out = t_capture_stdout(run_command, &call);
    free(out);
    EXPECT(session.n_items == 2);
    agent_session_free(&session);
}

static void test_goal_rejects_raw_mode(void)
{
    struct agent_session session = {.raw_mode = 1};
    struct agent_state state = {.session = &session};
    struct command_call call = {.state = &state, .argument = "fix tests"};
    char *out = t_capture_stdout(run_command, &call);
    EXPECT(strstr(out, "without --raw") != NULL);
    EXPECT(state.pending_prompt == NULL);
    EXPECT(session.n_items == 0);
    free(out);
    agent_session_free(&session);
}

int main(void)
{
    test_goal_command();
    test_goal_rejects_raw_mode();
    T_REPORT();
}
