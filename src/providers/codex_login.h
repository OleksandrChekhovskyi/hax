/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CODEX_LOGIN_H
#define HAX_PROVIDERS_CODEX_LOGIN_H

#include <jansson.h>

#include "providers/codex_auth.h"
#include "transport/http.h"

/* ChatGPT login lifecycle for the codex provider, independent of the codex CLI: the OpenAI device
 * flow obtains tokens, the credential store persists them under "codex", and refresh keeps them
 * rotating. Runs on the foreground thread. */

/* Interactive device login: prints the verification URL and user code, copies the code to the
 * clipboard, and polls until the browser approval lands. Returns 0 on success (result printed),
 * 1 on user cancellation, -1 on failure (reported). */
int codex_login_run(void);

/* Remove the stored login, revoking the refresh token best-effort. Returns 1 when a login was
 * removed, 0 when none was stored, -1 on failure (reported). */
int codex_logout_run(void);

enum codex_refresh_result {
    CODEX_REFRESH_FRESH,     /* `auth` is usable, possibly updated in place */
    CODEX_REFRESH_TRANSIENT, /* refresh did not complete; credentials retained for a later try */
    CODEX_REFRESH_DEAD,      /* removed, replaced by another account, or rejected; /login again */
};

/* Keep a hax-owned credential usable: refresh when the access token nears expiry, or
 * unconditionally with `force` after a 401, coordinating token rotation with concurrent hax
 * processes through the store. Borrowed credentials never refresh: FRESH without `force`, DEAD
 * with it. */
enum codex_refresh_result codex_login_ensure_fresh(struct codex_auth *auth, int force,
                                                   http_tick_cb tick, void *tick_user);

/* One line describing the current codex credential state for the login picker: the hax-owned
 * login, or the borrowed codex CLI credentials. NULL when neither exists. The caller frees. */
char *codex_login_status(void);

/* Whether a hax-owned login is stored; borrowed codex CLI credentials do not count. */
int codex_login_present(void);

/* Pure protocol helpers, exposed for tests. */

struct codex_device_auth {
    char *device_auth_id;
    char *user_code;
    long interval_s; /* poll interval, clamped to a sane range; defaulted when unreported */
};

/* Parse the device-authorization response. Returns 0 on success with owned fields, -1 otherwise. */
int codex_login_parse_usercode(const char *body, struct codex_device_auth *out);

void codex_device_auth_release(struct codex_device_auth *device_auth);

enum codex_poll_result {
    CODEX_POLL_AUTHORIZED,
    CODEX_POLL_PENDING,
    CODEX_POLL_SLOW_DOWN, /* pending; lengthen the poll interval */
    CODEX_POLL_FAILED,
};

/* Classify one poll response. On CODEX_POLL_AUTHORIZED the owned authorization code and
 * server-generated PKCE verifier are stored in the out parameters. */
enum codex_poll_result codex_login_classify_poll(long http_status, const char *body,
                                                 char **authorization_code, char **code_verifier);

/* Build the form-urlencoded authorization-code exchange body; all values percent-encoded. */
char *codex_login_build_exchange_body(const char *authorization_code, const char *code_verifier);

/* Build a credential-store entry from an exchange response, which must carry id_token,
 * access_token, and refresh_token; the account id is decoded from the token claims. Returns the
 * owned entry or NULL. */
json_t *codex_login_entry_from_exchange(const char *body);

/* Merge a refresh response into a store entry. Refresh responses may omit trailing fields;
 * present fields replace stored ones and account_id is preserved. Returns 0 when a new access
 * token was merged, -1 otherwise with the entry unchanged. */
int codex_login_apply_refresh(json_t *entry, const char *body);

/* Whether `candidate` is at least as fresh as `current`, ordered by JWT `exp`; an unreadable
 * expiry on either side counts as fresh, so opaque tokens stay adoptable. */
int codex_login_token_as_fresh(const char *candidate, const char *current);

/* Whether the access token's `exp` falls within `margin_s` seconds from now; an unreadable expiry
 * never counts as expiring. The margin expresses how long the caller's request must outlive the
 * token check. */
int codex_login_token_expiring(const char *access_token, long margin_s);

/* Whether a refresh response is an explicit terminal OAuth rejection of the grant. Only such
 * proof justifies treating a login as dead and removing it: proxies and gateways fabricate 4xx
 * statuses that say nothing about the token itself. */
int codex_login_refresh_rejected(long http_status, const char *body);

#endif /* HAX_PROVIDERS_CODEX_LOGIN_H */
