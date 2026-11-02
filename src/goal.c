/* SPDX-License-Identifier: MIT */
#include "goal.h"

#include <ctype.h>
#include <string.h>

#include "agent_core.h"
#include "provider.h"
#include "xalloc.h"

const char *goal_current(const struct agent_session *session)
{
    if (session->raw_mode)
        return NULL;
    for (size_t i = session->n_items; i-- > 0;) {
        const struct item *item = &session->items[i];
        if (item->origin == ITEM_ORIGIN_GOAL_END)
            return NULL;
        if (item->origin == ITEM_ORIGIN_GOAL)
            return item->text;
    }
    return NULL;
}

char *goal_prompt(const char *objective)
{
    return xasprintf(
        "Continue working toward this goal:\n\n%s\n\n"
        "Take concrete steps and verify the result. Do not stop at a plan or progress report. "
        "When the goal is achieved, explain the result and verification, then end with "
        "[goal complete] on its own final line. If you cannot safely proceed without user "
        "input, permission, credentials, or an external dependency, explain the blocker and "
        "end with [goal blocked] on its own final line. Do not claim completion without "
        "evidence. Do not expand the scope or bypass safety constraints. Otherwise keep working.",
        objective);
}

int goal_finish_response(struct agent_session *session, size_t from)
{
    for (size_t i = session->n_items; i-- > from;) {
        struct item *item = &session->items[i];
        if (item->kind != ITEM_ASSISTANT_MESSAGE || !item->text)
            continue;
        const char *text = item->text;
        size_t end = strlen(text);
        while (end && isspace((unsigned char)text[end - 1]))
            end--;
        size_t start = end;
        while (start && text[start - 1] != '\n')
            start--;
        size_t length = end - start;
        if ((length == strlen("[goal complete]") &&
             strncmp(text + start, "[goal complete]", length) == 0) ||
            (length == strlen("[goal blocked]") &&
             strncmp(text + start, "[goal blocked]", length) == 0)) {
            item->origin = ITEM_ORIGIN_GOAL_END;
            return 1;
        }
        return 0;
    }
    return 0;
}
