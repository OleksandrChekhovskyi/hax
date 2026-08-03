/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CODEX_AUTH_H
#define HAX_PROVIDERS_CODEX_AUTH_H

#include <jansson.h>

/* Credentials read from the codex CLI's ~/.codex/auth.json. All fields are owned; `email` is NULL
 * when the id_token is absent or carries no email claim. */
struct codex_auth {
    char *access_token;
    char *account_id;
    char *email;
};

enum codex_auth_status {
    CODEX_AUTH_OK,
    CODEX_AUTH_NO_FILE,
    CODEX_AUTH_BAD_JSON,
    CODEX_AUTH_NO_TOKENS,
};

/* Load credentials from ~/.codex/auth.json. On failure `auth` is left zeroed. When `detail` is
 * non-NULL it receives allocated context the caller frees: the resolved path for
 * CODEX_AUTH_NO_FILE, the parser message for CODEX_AUTH_BAD_JSON, NULL otherwise. */
enum codex_auth_status codex_auth_load(struct codex_auth *auth, char **detail);

/* Read credentials out of a parsed auth.json document. Returns CODEX_AUTH_NO_TOKENS unless both
 * tokens.access_token and tokens.account_id are present and non-empty. Copies what it reads, so
 * `auth` outlives `root`. */
enum codex_auth_status codex_auth_from_json(const json_t *root, struct codex_auth *auth);

/* A short description suitable for the provider picker; NULL for CODEX_AUTH_OK. */
const char *codex_auth_status_reason(enum codex_auth_status status);

void codex_auth_release(struct codex_auth *auth);

/* Decode the email claim from a JWT payload without verifying the signature: this is an
 * informational label, not authentication. Some login flows carry the email only in the namespaced
 * profile claim. Returns NULL when the token is malformed or has no email; the caller owns the
 * result. */
char *codex_jwt_email(const char *jwt);

#endif /* HAX_PROVIDERS_CODEX_AUTH_H */
