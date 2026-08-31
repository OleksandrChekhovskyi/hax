/* SPDX-License-Identifier: MIT */
#include "providers/codex_auth.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "cred_store.h"
#include "diag.h"
#include "xalloc.h"
#include "providers/codex_login.h"
#include "providers/http_provider.h"
#include "system/fs.h"
#include "system/path.h"
#include "text/base64.h"
#include "transport/http.h"

#define CODEX_CLI_AUTH_PATH "~/.codex/auth.json"

json_t *codex_jwt_payload(const char *jwt)
{
    if (!jwt || !*jwt)
        return NULL;

    const char *payload_start = strchr(jwt, '.');
    if (!payload_start)
        return NULL;
    payload_start++;

    const char *payload_end = strchr(payload_start, '.');
    if (!payload_end)
        return NULL;

    unsigned char *payload =
        base64url_decode(payload_start, (size_t)(payload_end - payload_start), NULL);
    if (!payload)
        return NULL;

    json_t *root = json_loads((char *)payload, 0, NULL);
    free(payload);
    return root;
}

char *codex_jwt_email(const char *jwt)
{
    json_t *payload = codex_jwt_payload(jwt);
    if (!payload)
        return NULL;

    const char *email = json_string_value(json_object_get(payload, "email"));
    if (!email || !*email) {
        json_t *profile = json_object_get(payload, "https://api.openai.com/profile");
        email = json_string_value(json_object_get(profile, "email"));
    }

    char *result = email && *email ? xstrdup(email) : NULL;
    json_decref(payload);
    return result;
}

long codex_jwt_exp(const char *jwt)
{
    json_t *payload = codex_jwt_payload(jwt);
    if (!payload)
        return 0;

    json_t *exp = json_object_get(payload, "exp");
    long result = json_is_number(exp) ? (long)json_number_value(exp) : 0;
    json_decref(payload);
    return result > 0 ? result : 0;
}

char *codex_jwt_account_id(const char *jwt)
{
    json_t *payload = codex_jwt_payload(jwt);
    if (!payload)
        return NULL;

    json_t *auth_claim = json_object_get(payload, "https://api.openai.com/auth");
    const char *account_id = json_string_value(json_object_get(auth_claim, "chatgpt_account_id"));
    char *result = account_id && *account_id ? xstrdup(account_id) : NULL;
    json_decref(payload);
    return result;
}

enum codex_auth_status codex_auth_from_json(const json_t *root, struct codex_auth *auth)
{
    memset(auth, 0, sizeof(*auth));

    json_t *tokens = json_object_get(root, "tokens");
    const char *access_token = json_string_value(json_object_get(tokens, "access_token"));
    const char *account_id = json_string_value(json_object_get(tokens, "account_id"));
    if (!access_token || !*access_token || !account_id || !*account_id)
        return CODEX_AUTH_NO_TOKENS;

    auth->access_token = xstrdup(access_token);
    auth->account_id = xstrdup(account_id);
    auth->email = codex_jwt_email(json_string_value(json_object_get(tokens, "id_token")));
    auth->source = CODEX_AUTH_SOURCE_CODEX_CLI;
    return CODEX_AUTH_OK;
}

enum codex_auth_status codex_auth_from_store_entry(const json_t *entry, struct codex_auth *auth)
{
    memset(auth, 0, sizeof(*auth));

    const char *access_token = json_string_value(json_object_get(entry, "access_token"));
    const char *refresh_token = json_string_value(json_object_get(entry, "refresh_token"));
    const char *account_id = json_string_value(json_object_get(entry, "account_id"));
    if (!access_token || !*access_token || !refresh_token || !*refresh_token || !account_id ||
        !*account_id)
        return CODEX_AUTH_NO_TOKENS;

    auth->access_token = xstrdup(access_token);
    auth->refresh_token = xstrdup(refresh_token);
    auth->account_id = xstrdup(account_id);
    auth->email = codex_jwt_email(json_string_value(json_object_get(entry, "id_token")));
    auth->source = CODEX_AUTH_SOURCE_HAX;
    return CODEX_AUTH_OK;
}

static enum codex_auth_status load_codex_cli(struct codex_auth *auth, char **detail)
{
    char *path = path_expand_home(CODEX_CLI_AUTH_PATH);
    char *contents = fs_read_file(path, NULL);
    if (!contents) {
        if (detail)
            *detail = path;
        else
            free(path);
        return CODEX_AUTH_NO_FILE;
    }
    free(path);

    json_error_t error;
    json_t *root = json_loads(contents, 0, &error);
    free(contents);
    if (!root) {
        if (detail)
            *detail = xstrdup(error.text);
        return CODEX_AUTH_BAD_JSON;
    }

    enum codex_auth_status status = codex_auth_from_json(root, auth);
    json_decref(root);
    return status;
}

enum codex_auth_status codex_auth_load(struct codex_auth *auth, char **detail)
{
    memset(auth, 0, sizeof(*auth));
    if (detail)
        *detail = NULL;

    json_t *entry = cred_store_get("codex");
    if (entry) {
        enum codex_auth_status status = codex_auth_from_store_entry(entry, auth);
        json_decref(entry);
        /* A partial entry falls through to the CLI rather than blocking it. */
        if (status == CODEX_AUTH_OK)
            return status;
    }

    return load_codex_cli(auth, detail);
}

static int same_string(const char *a, const char *b)
{
    return a == b || (a && b && strcmp(a, b) == 0);
}

int codex_auth_equal(const struct codex_auth *a, const struct codex_auth *b)
{
    return same_string(a->access_token, b->access_token) &&
           same_string(a->account_id, b->account_id);
}

const char *codex_auth_status_reason(enum codex_auth_status status)
{
    switch (status) {
    case CODEX_AUTH_OK:
        return NULL;
    case CODEX_AUTH_BAD_JSON:
        return "auth.json not valid JSON";
    case CODEX_AUTH_NO_FILE:
    case CODEX_AUTH_NO_TOKENS:
        return "not logged in (use /login)";
    }
    return "not logged in (use /login)";
}

void codex_auth_release(struct codex_auth *auth)
{
    free(auth->access_token);
    free(auth->account_id);
    free(auth->email);
    free(auth->refresh_token);
    memset(auth, 0, sizeof(*auth));
}

/* Only the codex CLI can refresh a borrowed token; hax re-reads auth.json on the next request. */
#define CODEX_TOKEN_EXPIRED_CLI    "codex CLI token expired — rerun `codex`, or use /login"
#define CODEX_TOKEN_EXPIRED_HAX    "codex login expired — run /login again"
#define CODEX_TOKEN_REFRESH_FAILED "could not refresh the codex login — retry, or run /login"
/* The live auth was cleared by /logout and no fallback credentials have appeared since. */
#define CODEX_NOT_LOGGED_IN "codex is not logged in — run /login"
#define CODEX_ACCOUNT_CHANGED                                                                      \
    "the codex login belongs to a different account — run /login or /provider to switch"

struct codex_auth_session {
    struct codex_auth auth;
    /* Set when a request with borrowed credentials is rejected as unauthenticated, so the next one
     * re-reads the codex CLI's auth.json and picks up a token it refreshed meanwhile. hax-owned
     * credentials refresh through codex_login_ensure_fresh instead. */
    int auth_stale;
    /* The last forced recovery failed transiently, so its 401 advises a retry, not /login. */
    int refresh_transient;
    /* Account this session was constructed for (or explicitly switched to); credentials for any
     * other account are never adopted implicitly, even after the live auth is cleared. */
    char *account_pin;
    /* Reloading found credentials for a different account, so requests report that instead of
     * "not logged in". */
    int account_blocked;
};

/* Clearing the mark only once a different token is adopted keeps callers that cannot re-mark from
 * consuming it: model probes discard their HTTP status, so a probe's 401 is invisible here. */
static void reload_auth_if_stale(struct codex_auth_session *session)
{
    if (!session->auth_stale)
        return;

    struct codex_auth refreshed;
    if (codex_auth_load(&refreshed, NULL) != CODEX_AUTH_OK)
        return;

    if (codex_auth_equal(&session->auth, &refreshed)) {
        codex_auth_release(&refreshed);
        return;
    }

    codex_auth_release(&session->auth);
    session->auth = refreshed;
    session->auth_stale = 0;
}

static int codex_auth_prepare(void *auth, int allow_refresh, http_tick_cb tick, void *tick_user)
{
    struct codex_auth_session *session = auth;
    if (!session->auth.access_token) {
        /* /logout cleared the live auth; a later /login — possibly in another hax process — or a
         * codex CLI login can restore it, but only for the pinned account: switching whose
         * account a conversation is sent under takes an explicit /login or /provider action. */
        codex_auth_load(&session->auth, NULL);
        session->account_blocked = 0;
        if (session->auth.access_token && session->account_pin && session->auth.account_id &&
            strcmp(session->auth.account_id, session->account_pin) != 0) {
            codex_auth_release(&session->auth);
            session->account_blocked = 1;
        }
        return session->auth.access_token ? 0 : -1;
    }

    if (session->auth.source == CODEX_AUTH_SOURCE_HAX) {
        /* A failed proactive refresh is not terminal here: the request's own 401 recovery
         * reports it if the stale token really is rejected. */
        if (allow_refresh)
            codex_login_ensure_fresh(&session->auth, 0, tick, tick_user);
    } else {
        reload_auth_if_stale(session);
    }
    return 0;
}

static char **codex_auth_headers(const void *auth, const char *session_id, int streaming)
{
    const struct codex_auth_session *session = auth;
    char *authorization = xasprintf("Authorization: Bearer %s", session->auth.access_token);
    char *account = xasprintf("chatgpt-account-id: %s", session->auth.account_id);
    char *session_header = NULL;
    char *request_id = NULL;
    const char *fixed[6];
    size_t n_fixed = 0;
    fixed[n_fixed++] = authorization;
    fixed[n_fixed++] = account;
    if (streaming) {
        session_header = xasprintf("session-id: %s", session_id);
        request_id = xasprintf("x-client-request-id: %s", session_id);
        fixed[n_fixed++] = session_header;
        fixed[n_fixed++] = request_id;
        fixed[n_fixed++] = "OpenAI-Beta: responses=experimental";
    } else {
        fixed[n_fixed++] = "Accept: application/json";
    }
    fixed[n_fixed] = NULL;

    char **headers = string_array_concat(fixed, NULL);
    free(authorization);
    free(account);
    free(session_header);
    free(request_id);
    return headers;
}

/* One forced refresh per operation after a 401 on hax-owned credentials. Returns 1 when the
 * request should be retried with rebuilt headers. */
static int codex_auth_recover(void *auth, int *retried, http_tick_cb tick, void *tick_user)
{
    struct codex_auth_session *session = auth;
    if (*retried || session->auth.source != CODEX_AUTH_SOURCE_HAX)
        return 0;
    *retried = 1;
    session->refresh_transient = 0;
    switch (codex_login_ensure_fresh(&session->auth, 1, tick, tick_user)) {
    case CODEX_REFRESH_FRESH:
        return 1;
    case CODEX_REFRESH_TRANSIENT:
        /* Credentials stay; the next attempt refreshes again. */
        session->refresh_transient = 1;
        return 0;
    case CODEX_REFRESH_DEAD:
        break;
    }

    /* The managed login is dead or was removed by a concurrent /logout. Retry with what the
     * canonical load finds — possibly the codex CLI fallback — but never resend the failed
     * request across an account boundary: credentials outside the pinned account are left for
     * the user to adopt explicitly. Otherwise clear the live auth so later requests report the
     * logged-out state rather than resending the dead token. */
    struct codex_auth fallback;
    if (codex_auth_load(&fallback, NULL) == CODEX_AUTH_OK &&
        !codex_auth_equal(&fallback, &session->auth) && fallback.account_id &&
        session->account_pin && strcmp(fallback.account_id, session->account_pin) == 0) {
        codex_auth_release(&session->auth);
        session->auth = fallback;
        return 1;
    }
    codex_auth_release(&fallback);
    codex_auth_release(&session->auth);
    return 0;
}

static char *codex_auth_unauthorized_message(void *auth)
{
    struct codex_auth_session *session = auth;
    if (!session->auth.access_token)
        return xstrdup(session->account_blocked ? CODEX_ACCOUNT_CHANGED : CODEX_NOT_LOGGED_IN);
    /* Record a request-level 401 so the next request re-evaluates borrowed credentials. */
    if (session->auth.source == CODEX_AUTH_SOURCE_CODEX_CLI)
        session->auth_stale = 1;
    if (session->auth.source != CODEX_AUTH_SOURCE_HAX)
        return xstrdup(CODEX_TOKEN_EXPIRED_CLI);
    return xstrdup(session->refresh_transient ? CODEX_TOKEN_REFRESH_FAILED
                                              : CODEX_TOKEN_EXPIRED_HAX);
}

static void codex_auth_destroy(void *auth)
{
    struct codex_auth_session *session = auth;
    codex_auth_release(&session->auth);
    free(session->account_pin);
    free(session);
}

static const struct http_auth_ops CODEX_AUTH_OPS = {
    .prepare = codex_auth_prepare,
    .headers = codex_auth_headers,
    .recover = codex_auth_recover,
    .unauthorized_message = codex_auth_unauthorized_message,
    .destroy = codex_auth_destroy,
};

int codex_auth_source(const struct provider_def *def, struct http_auth_source *out)
{
    (void)def;
    struct codex_auth loaded;
    char *detail = NULL;
    enum codex_auth_status status = codex_auth_load(&loaded, &detail);
    switch (status) {
    case CODEX_AUTH_OK:
        break;
    case CODEX_AUTH_NO_FILE:
        hax_err("no ChatGPT login found — run /login, or log in with the codex CLI (%s)", detail);
        free(detail);
        return -1;
    case CODEX_AUTH_BAD_JSON:
        hax_err("~/.codex/auth.json is not valid JSON: %s", detail);
        free(detail);
        return -1;
    case CODEX_AUTH_NO_TOKENS:
        hax_err("auth.json missing tokens.access_token or tokens.account_id");
        free(detail);
        return -1;
    }
    free(detail);

    struct codex_auth_session *session = xcalloc(1, sizeof(*session));
    session->auth = loaded;
    session->account_pin = xstrdup(loaded.account_id);
    out->ops = &CODEX_AUTH_OPS;
    out->state = session;
    return 0;
}

void codex_auth_session_reload(struct codex_auth_session *session)
{
    codex_auth_release(&session->auth);
    codex_auth_load(&session->auth, NULL);
    free(session->account_pin);
    session->account_pin = xstrdup(session->auth.account_id);
    session->account_blocked = 0;
    session->auth_stale = 0;
}

int codex_auth_session_expiring(const struct codex_auth_session *session, long margin_s)
{
    return session->auth.source == CODEX_AUTH_SOURCE_HAX &&
           codex_login_token_expiring(session->auth.access_token, margin_s);
}

const char *codex_auth_session_email(const struct codex_auth_session *session)
{
    return session->auth.email;
}
