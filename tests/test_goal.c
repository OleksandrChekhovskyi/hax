/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "goal.h"
#include "harness.h"
#include "provider.h"
#include "xalloc.h"

static void test_goal_tracks_record(void)
{
    struct agent_session session = {0};
    EXPECT(goal_current(&session) == NULL);
    agent_session_append(&session, (struct item){.kind = ITEM_USER_MESSAGE,
                                                 .text = xstrdup("fix tests"),
                                                 .origin = ITEM_ORIGIN_GOAL});
    EXPECT_STR_EQ(goal_current(&session), "fix tests");
    EXPECT(item_is_typed_prompt(&session.items[0]));
    agent_session_append(&session, (struct item){.kind = ITEM_USER_MESSAGE,
                                                 .text = xstrdup("summary"),
                                                 .origin = ITEM_ORIGIN_COMPACT_SEED});
    EXPECT_STR_EQ(goal_current(&session), "fix tests");
    agent_session_append(&session, (struct item){.kind = ITEM_USER_MESSAGE,
                                                 .text = xstrdup("cancelled"),
                                                 .origin = ITEM_ORIGIN_GOAL_END});
    EXPECT(goal_current(&session) == NULL);
    EXPECT(!item_is_typed_prompt(&session.items[2]));
    agent_session_append(&session, (struct item){.kind = ITEM_USER_MESSAGE,
                                                 .text = xstrdup("new goal"),
                                                 .origin = ITEM_ORIGIN_GOAL});
    EXPECT_STR_EQ(goal_current(&session), "new goal");
    agent_session_retire(&session, 2);
    EXPECT_STR_EQ(goal_current(&session), "fix tests");
    agent_session_reset(&session);
    EXPECT(goal_current(&session) == NULL);
    agent_session_free(&session);
}

static void test_goal_completion_is_final_assistant_line(void)
{
    const char *texts[] = {"verified\n[goal complete]", "blocked\n[goal blocked]\n \t",
                           "[goal complete]"};
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        struct agent_session session = {0};
        agent_session_append(
            &session, (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup(texts[i])});
        EXPECT(goal_finish_response(&session, 0));
        EXPECT(session.items[0].origin == ITEM_ORIGIN_GOAL_END);
        agent_session_free(&session);
    }
    const char *unfinished[] = {"",
                                "working",
                                "quoted [goal complete]",
                                "[goal complete]\nmore",
                                "[goal complete-ish]",
                                "[goal blocked] not yet"};
    for (size_t i = 0; i < sizeof(unfinished) / sizeof(unfinished[0]); i++) {
        struct agent_session session = {0};
        agent_session_append(&session, (struct item){.kind = ITEM_ASSISTANT_MESSAGE,
                                                     .text = xstrdup(unfinished[i])});
        EXPECT(!goal_finish_response(&session, 0));
        EXPECT(session.items[0].origin == ITEM_ORIGIN_NONE);
        agent_session_free(&session);
    }
    struct agent_session session = {0};
    agent_session_append(
        &session, (struct item){.kind = ITEM_USER_MESSAGE, .text = xstrdup("[goal complete]")});
    EXPECT(!goal_finish_response(&session, 0));
    agent_session_append(&session, (struct item){.kind = ITEM_ASSISTANT_MESSAGE,
                                                 .text = xstrdup("[goal complete]")});
    agent_session_append(
        &session, (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup("still working")});
    EXPECT(!goal_finish_response(&session, 0));
    EXPECT(!goal_finish_response(&session, session.n_items));
    agent_session_free(&session);
}

static void test_goal_prompt_contains_objective_and_protocol(void)
{
    char *prompt = goal_prompt("run tests\nthen fix failures");
    EXPECT(strstr(prompt, "run tests\nthen fix failures") != NULL);
    EXPECT(strstr(prompt, "[goal complete]") != NULL);
    EXPECT(strstr(prompt, "[goal blocked]") != NULL);
    free(prompt);
}

int main(void)
{
    test_goal_tracks_record();
    test_goal_completion_is_final_assistant_line();
    test_goal_prompt_contains_objective_and_protocol();
    T_REPORT();
}
