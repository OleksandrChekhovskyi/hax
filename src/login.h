/* SPDX-License-Identifier: MIT */
#ifndef HAX_LOGIN_H
#define HAX_LOGIN_H

struct agent_state;

/* /login: pick a login-capable provider (or take its id as `argument`) and run its flow. On
 * success the live provider adopts the new credentials in place. */
void login_command(struct agent_state *state, const char *argument);

/* /logout: remove a hax-managed login, picking when more than one exists. Borrowed credentials
 * (the codex CLI's file) are never touched. */
void logout_command(struct agent_state *state, const char *argument);

#endif /* HAX_LOGIN_H */
