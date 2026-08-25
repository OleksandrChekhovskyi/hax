/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CODEX_AUTH_H
#define HAX_PROVIDERS_CODEX_AUTH_H

#include <jansson.h>

enum codex_auth_source {
    CODEX_AUTH_SOURCE_HAX,       /* hax's own credential store; hax manages refresh */
    CODEX_AUTH_SOURCE_CODEX_CLI, /* borrowed read-only from ~/.codex/auth.json */
};

/* ChatGPT credentials for the codex provider. All fields are owned; `email` is NULL when the
 * id_token is absent or carries no email claim. `refresh_token` is NULL for borrowed credentials:
 * refreshing with the codex CLI's rotating token would invalidate its session, so those tokens are
 * used as-is until the CLI renews them. */
struct codex_auth {
    char *access_token;
    char *account_id;
    char *email;
    char *refresh_token;
    enum codex_auth_source source;
};

enum codex_auth_status {
    CODEX_AUTH_OK,
    CODEX_AUTH_NO_FILE,
    CODEX_AUTH_BAD_JSON,
    CODEX_AUTH_NO_TOKENS,
};

/* Load credentials, preferring hax's own store over the codex CLI's ~/.codex/auth.json. On failure
 * `auth` is left zeroed and the status describes the CLI fallback, the last source tried. When
 * `detail` is non-NULL it receives allocated context the caller frees: the resolved CLI path for
 * CODEX_AUTH_NO_FILE, the parser message for CODEX_AUTH_BAD_JSON, NULL otherwise. */
enum codex_auth_status codex_auth_load(struct codex_auth *auth, char **detail);

/* Read borrowed credentials out of a parsed ~/.codex/auth.json document. Returns
 * CODEX_AUTH_NO_TOKENS unless both tokens.access_token and tokens.account_id are present and
 * non-empty. Copies what it reads, so `auth` outlives `root`. */
enum codex_auth_status codex_auth_from_json(const json_t *root, struct codex_auth *auth);

/* Read hax-owned credentials out of a credential-store entry. Returns CODEX_AUTH_NO_TOKENS unless
 * access_token, refresh_token, and account_id are present and non-empty. */
enum codex_auth_status codex_auth_from_store_entry(const json_t *entry, struct codex_auth *auth);

/* Compare the values sent as authentication headers, access_token and account_id. The email is an
 * informational label and is ignored, so a reload that changes only it counts as unchanged. */
int codex_auth_equal(const struct codex_auth *a, const struct codex_auth *b);

struct provider_def;     /* providers/registry.h */
struct http_auth_source; /* providers/http_provider.h */

/* Per-provider credential session over the loaded auth: staleness reloads and forced refresh,
 * account pinning across logins, and the user-facing messages for auth failures. Generic
 * operations run through the returned ops; the accessors below cover what codex's own
 * provider hooks need beyond them. */
struct codex_auth_session;

/* Auth-source hook for the codex def: open a session from the hax login or ~/.codex/auth.json
 * as `out`'s state, or report why none is usable and return non-zero. */
int codex_auth_source(const struct provider_def *def, struct http_auth_source *out);

/* Re-resolve credentials after /login or /logout. When none remain the auth stays cleared so
 * requests report "not logged in" rather than reusing a removed token; this explicit action is
 * also what may re-pin the session to a different account. */
void codex_auth_session_reload(struct codex_auth_session *session);

/* Whether the current token expires within `margin_s` seconds. Only hax-owned tokens report
 * expiry; borrowed CLI tokens never refresh here, so they never count as expiring. */
int codex_auth_session_expiring(const struct codex_auth_session *session, long margin_s);

/* Account email for display, or NULL; borrowed until the next credential change. */
const char *codex_auth_session_email(const struct codex_auth_session *session);

/* A short description suitable for the provider picker; NULL for CODEX_AUTH_OK. */
const char *codex_auth_status_reason(enum codex_auth_status status);

void codex_auth_release(struct codex_auth *auth);

/* Decode a JWT payload without verifying the signature: claims read this way are routing and
 * display inputs, not authentication. Returns an owned object, or NULL for malformed input. */
json_t *codex_jwt_payload(const char *jwt);

/* Decode the email claim from a JWT. Some login flows carry the email only in the namespaced
 * profile claim. Returns NULL when the token is malformed or has no email; the caller owns the
 * result. */
char *codex_jwt_email(const char *jwt);

/* The `exp` claim as Unix seconds, or 0 when absent or malformed. */
long codex_jwt_exp(const char *jwt);

/* The ChatGPT account id claim (`https://api.openai.com/auth`.chatgpt_account_id), owned, or
 * NULL when absent. */
char *codex_jwt_account_id(const char *jwt);

#endif /* HAX_PROVIDERS_CODEX_AUTH_H */
