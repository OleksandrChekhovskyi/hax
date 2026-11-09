/* SPDX-License-Identifier: MIT */
#include "providers/anthropic_auth.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "cred_store.h"
#include "diag.h"
#include "provider.h"
#include "trace.h"
#include "xalloc.h"
#include "providers/anthropic_login.h"
#include "providers/http_provider.h"
#include "providers/provider_config.h"
#include "transport/http.h"

#define ANTHROPIC_REFRESH_MARGIN_S 600

enum anthropic_auth_status {
    ANTHROPIC_AUTH_READY,
    ANTHROPIC_AUTH_LOGGED_OUT,
    ANTHROPIC_AUTH_CHANGED,
    ANTHROPIC_AUTH_TRANSIENT,
    ANTHROPIC_AUTH_REJECTED,
};

struct anthropic_auth_session {
    json_t *entry;
    char *api_key;
    char *version;
    char *login_pin; /* a new /login must not silently switch an existing conversation's account */
    enum anthropic_auth_status status;
};

static const char *entry_string(const json_t *entry, const char *key)
{
    return json_string_value(json_object_get(entry, key));
}

static json_int_t entry_expiry(const json_t *entry)
{
    return json_integer_value(json_object_get(entry, "expires_at"));
}

static json_int_t entry_generation(const json_t *entry)
{
    return json_integer_value(json_object_get(entry, "generation"));
}

static int same_login(const struct anthropic_auth_session *session, const json_t *entry)
{
    const char *id = entry_string(entry, "login_id");
    return session->login_pin && id && strcmp(id, session->login_pin) == 0;
}

static void adopt_entry(struct anthropic_auth_session *session, json_t *entry)
{
    json_incref(entry);
    json_decref(session->entry);
    session->entry = entry;
    session->status = ANTHROPIC_AUTH_READY;
    trace_register_secret(entry_string(entry, "access_token"));
    trace_register_secret(entry_string(entry, "refresh_token"));
}

struct refresh_tx {
    struct anthropic_auth_session *session;
    http_tick_cb tick;
    void *tick_user;
    int force;
    int ran;
    json_t *adopted;
};

/* Rotation must finish even after cancellation: the server may have spent the old refresh token. */
static int rotation_tick(void *user)
{
    struct refresh_tx *tx = user;
    if (tx->tick)
        tx->tick(tx->tick_user);
    return 0;
}

static enum cred_store_verdict refresh_transaction(json_t *entry, json_t **replacement, void *user)
{
    struct refresh_tx *tx = user;
    struct anthropic_auth_session *session = tx->session;
    tx->ran = 1;
    if (!anthropic_login_entry_valid(entry)) {
        session->status = ANTHROPIC_AUTH_LOGGED_OUT;
        return CRED_STORE_KEEP;
    }
    if (!same_login(session, entry)) {
        session->status = ANTHROPIC_AUTH_CHANGED;
        return CRED_STORE_KEEP;
    }
    const char *disk_access = entry_string(entry, "access_token");
    const char *current_access = entry_string(session->entry, "access_token");
    /* Token lifetimes may shrink on renewal; only the rotation generation orders credentials. */
    int as_fresh = entry_generation(entry) >= entry_generation(session->entry);
    if (as_fresh && strcmp(disk_access, current_access) != 0 &&
        (tx->force || entry_expiry(entry) > (json_int_t)time(NULL) + ANTHROPIC_REFRESH_MARGIN_S)) {
        tx->adopted = json_incref(entry);
        return CRED_STORE_KEEP;
    }
    /* If a previous persist failed, memory holds the newer refresh token of this same login. */
    const json_t *freshest = as_fresh ? entry : session->entry;
    char *body = anthropic_login_refresh_body(entry_string(freshest, "refresh_token"));
    char *response = NULL;
    long status = 0;
    int result = anthropic_login_token_request(body, rotation_tick, tx, &response, &status);
    free(body);
    if (result == 0 && status >= 200 && status < 300)
        tx->adopted = anthropic_login_parse_token(response, freshest);
    if (tx->adopted) {
        *replacement = json_incref(tx->adopted);
        free(response);
        return CRED_STORE_WRITE;
    }
    int rejected = result == 0 && anthropic_login_refresh_rejected(status, response);
    free(response);
    session->status = rejected ? ANTHROPIC_AUTH_REJECTED : ANTHROPIC_AUTH_TRANSIENT;
    return rejected ? CRED_STORE_REMOVE : CRED_STORE_KEEP;
}

static int refresh_session(struct anthropic_auth_session *session, int force, http_tick_cb tick,
                           void *tick_user)
{
    struct refresh_tx tx = {
        .session = session, .force = force, .tick = tick, .tick_user = tick_user};
    int stored = cred_store_update("anthropic", refresh_transaction, &tx);
    if (!tx.adopted) {
        if (!tx.ran)
            session->status = ANTHROPIC_AUTH_TRANSIENT;
        return -1;
    }
    adopt_entry(session, tx.adopted);
    json_decref(tx.adopted);
    if (stored < 0)
        hax_warn("cannot persist refreshed Anthropic login; this process retains the new token");
    return 0;
}

static int anthropic_auth_prepare(void *auth, int allow_refresh, http_tick_cb tick, void *tick_user)
{
    struct anthropic_auth_session *session = auth;
    if (!session->login_pin)
        return session->api_key ? 0 : -1;
    json_t *stored = cred_store_get("anthropic");
    if (!anthropic_login_entry_valid(stored) || !same_login(session, stored)) {
        session->status = anthropic_login_entry_valid(stored) ? ANTHROPIC_AUTH_CHANGED
                                                              : ANTHROPIC_AUTH_LOGGED_OUT;
        json_decref(stored);
        return -1;
    }
    if (entry_generation(stored) >= entry_generation(session->entry))
        adopt_entry(session, stored);
    json_decref(stored);
    json_int_t expiry = entry_expiry(session->entry);
    if (!allow_refresh)
        return expiry > (json_int_t)time(NULL) + 60 ? 0 : -1;
    if (expiry > (json_int_t)time(NULL) + ANTHROPIC_REFRESH_MARGIN_S)
        return 0;
    int result = refresh_session(session, 0, tick, tick_user);
    if (result == 0)
        return 0;
    /* A transient refresh error need not prevent using a still-valid access token. */
    return session->status == ANTHROPIC_AUTH_TRANSIENT && expiry > (json_int_t)time(NULL) ? 0 : -1;
}

static char **anthropic_auth_headers(const void *auth, const char *session_id, int streaming)
{
    (void)session_id;
    (void)streaming;
    const struct anthropic_auth_session *session = auth;
    char **headers = xcalloc(4, sizeof(*headers));
    headers[0] =
        session->login_pin
            ? xasprintf("Authorization: Bearer %s", entry_string(session->entry, "access_token"))
            : xasprintf("x-api-key: %s", session->api_key ? session->api_key : "");
    headers[1] = xasprintf("anthropic-version: %s", session->version);
    if (session->login_pin)
        headers[2] = xstrdup("anthropic-beta: oauth-2025-04-20");
    return headers;
}

static const char *anthropic_auth_system_identity(const void *auth)
{
    const struct anthropic_auth_session *session = auth;
    /* OAuth rejects requests without this exact standalone system block with a generic 429. */
    return session->login_pin ? "You are Claude Code, Anthropic's official CLI for Claude." : NULL;
}

static int anthropic_auth_recover(void *auth, int *retried, http_tick_cb tick, void *tick_user)
{
    if (*retried)
        return 0;
    *retried = 1;
    struct anthropic_auth_session *session = auth;
    return session->login_pin && refresh_session(session, 1, tick, tick_user) == 0;
}

static char *anthropic_auth_unauthorized_message(void *auth)
{
    struct anthropic_auth_session *session = auth;
    if (!session->login_pin && session->api_key)
        return xstrdup("Anthropic API key was rejected — check ANTHROPIC_API_KEY");
    switch (session->status) {
    case ANTHROPIC_AUTH_LOGGED_OUT:
        return xstrdup("Anthropic is not logged in — run /login anthropic");
    case ANTHROPIC_AUTH_CHANGED:
        return xstrdup("the Anthropic login changed — use /provider to adopt it explicitly");
    case ANTHROPIC_AUTH_TRANSIENT:
        return xstrdup("could not refresh the Anthropic login — retry, or run /login anthropic");
    case ANTHROPIC_AUTH_READY:
    case ANTHROPIC_AUTH_REJECTED:
        return xstrdup("Anthropic login expired or was rejected — run /login anthropic again");
    }
    return xstrdup("Anthropic authentication failed — run /login anthropic");
}

static void anthropic_auth_destroy(void *auth)
{
    struct anthropic_auth_session *session = auth;
    json_decref(session->entry);
    free(session->login_pin);
    free(session->api_key);
    free(session->version);
    free(session);
}

static const struct http_auth_ops ANTHROPIC_AUTH_OPS = {
    .prepare = anthropic_auth_prepare,
    .headers = anthropic_auth_headers,
    .system_identity = anthropic_auth_system_identity,
    .recover = anthropic_auth_recover,
    .unauthorized_message = anthropic_auth_unauthorized_message,
    .destroy = anthropic_auth_destroy,
};

static void reload_session(struct anthropic_auth_session *session)
{
    json_t *entry = cred_store_get("anthropic");
    json_decref(session->entry);
    session->entry = NULL;
    free(session->login_pin);
    session->login_pin = NULL;
    if (anthropic_login_entry_valid(entry)) {
        adopt_entry(session, entry);
        session->login_pin = xstrdup(entry_string(entry, "login_id"));
    } else {
        session->status = ANTHROPIC_AUTH_LOGGED_OUT;
    }
    json_decref(entry);
}

int anthropic_auth_source(const struct provider_def *def, struct http_auth_source *out)
{
    (void)def;
    const char *key = provider_api_key("providers.anthropic", "ANTHROPIC_API_KEY");
    if (!anthropic_login_present() && !key) {
        hax_err("no Anthropic credentials — run /login anthropic or set ANTHROPIC_API_KEY");
        return -1;
    }
    struct anthropic_auth_session *session = xcalloc(1, sizeof(*session));
    session->api_key = xstrdup(key);
    trace_register_secret(key);
    const char *version = config_scoped_str("providers.anthropic", "version");
    session->version = xstrdup(version && *version ? version : "2023-06-01");
    reload_session(session);
    out->ops = &ANTHROPIC_AUTH_OPS;
    out->state = session;
    return 0;
}

void anthropic_provider_reload_auth(struct provider *provider)
{
    reload_session(http_provider_auth(provider)->state);
}

void anthropic_prepare_availability(const struct provider_def *def,
                                    struct provider_availability *out)
{
    (void)def;
    memset(out, 0, sizeof(*out));
    out->available = anthropic_login_present() ||
                     provider_api_key("providers.anthropic", "ANTHROPIC_API_KEY") != NULL;
    out->reason = out->available ? NULL : xstrdup("use /login anthropic or set ANTHROPIC_API_KEY");
}
