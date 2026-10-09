/* SPDX-License-Identifier: MIT */
#ifndef HAX_COMMANDS_LOGIN_H
#define HAX_COMMANDS_LOGIN_H

struct agent_state;

/* /login: pick a login-capable provider (or take its id as `argument`) and run its flow. On
 * success the live provider adopts the new credentials in place. */
void login_command(struct agent_state *state, const char *argument);

/* /logout: remove a hax-managed login, picking when more than one exists. Borrowed credentials
 * (the codex CLI's file) are never touched. */
void logout_command(struct agent_state *state, const char *argument);

/* Add the provider ids /login and /logout take, for Tab completion, in their picker order: every
 * provider with a login flow, and only those holding a hax-managed login. Reads the local
 * credential store but never waits on the network. */
struct completion;
void login_choices(struct completion *choices);
void logout_choices(struct completion *choices);

#endif /* HAX_COMMANDS_LOGIN_H */
