/* SPDX-License-Identifier: MIT */
#ifndef HAX_GOAL_H
#define HAX_GOAL_H

#include <stddef.h>

struct agent_session;

/* Borrow the active objective from the full record, including compacted history.
 * Raw sessions never activate goal mode. */
const char *goal_current(const struct agent_session *session);

/* Return owned instructions for pursuing the objective. */
char *goal_prompt(const char *objective);

/* On a clean, tool-free response, stamp its final assistant message when its last line
 * declares completion or a blocker. Returns true when goal mode ended. */
int goal_finish_response(struct agent_session *session, size_t from);

#endif /* HAX_GOAL_H */
