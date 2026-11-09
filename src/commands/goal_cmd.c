/* SPDX-License-Identifier: MIT */
#include "commands/goal_cmd.h"

#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "agent_core.h"
#include "goal.h"
#include "provider.h"
#include "xalloc.h"
#include "terminal/ui.h"
#include "text/display_safe.h"

void goal_command(struct agent_state *state, const char *argument)
{
    const char *objective = goal_current(state->session);
    if (!argument) {
        if (objective) {
            char *flat = flatten_for_display(objective);
            ui_note("goal: %s", flat);
            free(flat);
            ui_note("Enter continues a paused goal; /goal clear cancels goal mode");
        } else {
            ui_note("no active goal — start one with /goal <objective>");
        }
        return;
    }
    if (strcmp(argument, "clear") == 0) {
        if (!objective) {
            ui_note("no active goal");
            return;
        }
        agent_session_append(state->session,
                             (struct item){.kind = ITEM_USER_MESSAGE,
                                           .text = xstrdup("Goal mode cancelled by the user."),
                                           .origin = ITEM_ORIGIN_GOAL_END});
        agent_flush_logs(state->transcript, state->session_log, state->session->items,
                         state->session->n_items);
        ui_note("goal cleared");
        return;
    }
    if (state->session->raw_mode) {
        ui_error("/goal needs tools; start hax without --raw");
        return;
    }
    free(state->pending_prompt);
    state->pending_prompt = xstrdup(argument);
}
