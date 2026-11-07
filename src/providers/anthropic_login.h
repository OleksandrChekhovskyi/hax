/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_ANTHROPIC_LOGIN_H
#define HAX_PROVIDERS_ANTHROPIC_LOGIN_H

#include <jansson.h>

#include "transport/http.h"

int anthropic_login_run(void);  /* 0: saved, 1: cancelled, -1: reported failure */
int anthropic_logout_run(void); /* 1: removed locally, 0: absent, -1: reported failure */
int anthropic_login_present(void);
char *anthropic_login_status(void); /* owned, or NULL when absent */

/* Owned protocol values; a pasted code must include the matching #state suffix. */
char *anthropic_login_authorize_url(const char *challenge, const char *state);
char *anthropic_login_code_body(const char *input, const char *verifier, const char *state);
char *anthropic_login_refresh_body(const char *refresh_token);
json_t *anthropic_login_parse_token(const char *body, const json_t *previous);
int anthropic_login_entry_valid(const json_t *entry);
int anthropic_login_refresh_rejected(long status, const char *body);
int anthropic_login_token_request(const char *body, http_tick_cb tick, void *tick_user,
                                  char **response, long *status);

#endif /* HAX_PROVIDERS_ANTHROPIC_LOGIN_H */
