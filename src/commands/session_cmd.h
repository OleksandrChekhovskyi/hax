/* SPDX-License-Identifier: MIT */
#ifndef HAX_COMMANDS_SESSION_CMD_H
#define HAX_COMMANDS_SESSION_CMD_H

struct agent_state;

/* /session: print the session's identity, the live conversation, and its accounting as label rows
 * fitted to the display width. Totals describe the recorded conversation — undone user turns and
 * retried requests included — so a resumed session reports what the live one did. */
void session_command(struct agent_state *state);

#endif /* HAX_COMMANDS_SESSION_CMD_H */
