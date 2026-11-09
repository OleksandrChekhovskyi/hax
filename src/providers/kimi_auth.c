/* SPDX-License-Identifier: MIT */
#include "providers/kimi_auth.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cred_store.h"
#include "diag.h"
#include "provider.h"
#include "trace.h"
#include "version.h"
#include "xalloc.h"
#include "providers/http_provider.h"
#include "providers/kimi_login.h"
#include "transport/http.h"

#define KIMI_REFRESH_MARGIN_S 600

enum kimi_auth_status {
    KIMI_AUTH_READY,
    KIMI_AUTH_LOGGED_OUT,
    KIMI_AUTH_CHANGED,
    KIMI_AUTH_TRANSIENT,
    KIMI_AUTH_REJECTED,
};

struct kimi_auth_session {
    json_t *entry;
    char *login_pin; /* a new /login must not silently switch an existing conversation's account */
    enum kimi_auth_status status;
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

static int same_login(const struct kimi_auth_session *session, const json_t *entry)
{
    const char *id = entry_string(entry, "login_id");
    return session->login_pin && id && strcmp(id, session->login_pin) == 0;
}

static void adopt_entry(struct kimi_auth_session *session, json_t *entry)
{
    json_incref(entry);
    json_decref(session->entry);
    session->entry = entry;
    session->status = KIMI_AUTH_READY;
    trace_register_secret(entry_string(entry, "access_token"));
    trace_register_secret(entry_string(entry, "refresh_token"));
}

struct refresh_tx {
    struct kimi_auth_session *session;
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
    struct kimi_auth_session *session = tx->session;
    tx->ran = 1;
    if (!kimi_login_entry_valid(entry)) {
        session->status = KIMI_AUTH_LOGGED_OUT;
        return CRED_STORE_KEEP;
    }
    if (!same_login(session, entry)) {
        session->status = KIMI_AUTH_CHANGED;
        return CRED_STORE_KEEP;
    }
    const char *disk_access = entry_string(entry, "access_token");
    const char *current_access = entry_string(session->entry, "access_token");
    /* Token lifetimes may shrink on renewal; only the rotation generation orders credentials. */
    int as_fresh = entry_generation(entry) >= entry_generation(session->entry);
    if (as_fresh && strcmp(disk_access, current_access) != 0 &&
        (tx->force || entry_expiry(entry) > (json_int_t)time(NULL) + KIMI_REFRESH_MARGIN_S)) {
        tx->adopted = json_incref(entry);
        return CRED_STORE_KEEP;
    }
    /* If a previous persist failed, memory holds the newer refresh token of this same login. */
    const json_t *freshest = as_fresh ? entry : session->entry;
    char *body = kimi_login_grant_body("refresh_token", entry_string(freshest, "refresh_token"));
    char *response = NULL;
    long status = 0;
    int result = kimi_login_token_request(body, entry_string(freshest, "device_id"), rotation_tick,
                                          tx, &response, &status);
    free(body);
    if (result == 0 && status >= 200 && status < 300)
        tx->adopted = kimi_login_parse_token(response, freshest);
    if (tx->adopted) {
        *replacement = json_incref(tx->adopted);
        free(response);
        return CRED_STORE_WRITE;
    }
    int rejected = result == 0 && kimi_login_refresh_rejected(status, response);
    free(response);
    session->status = rejected ? KIMI_AUTH_REJECTED : KIMI_AUTH_TRANSIENT;
    return rejected ? CRED_STORE_REMOVE : CRED_STORE_KEEP;
}

static int refresh_session(struct kimi_auth_session *session, int force, http_tick_cb tick,
                           void *tick_user)
{
    struct refresh_tx tx = {
        .session = session, .force = force, .tick = tick, .tick_user = tick_user};
    int stored = cred_store_update("kimi", refresh_transaction, &tx);
    if (!tx.adopted) {
        if (!tx.ran)
            session->status = KIMI_AUTH_TRANSIENT;
        return -1;
    }
    adopt_entry(session, tx.adopted);
    json_decref(tx.adopted);
    if (stored < 0)
        hax_warn("cannot persist refreshed Kimi login; this process retains the new token");
    return 0;
}

static int kimi_auth_prepare(void *auth, int allow_refresh, http_tick_cb tick, void *tick_user)
{
    struct kimi_auth_session *session = auth;
    json_t *stored = cred_store_get("kimi");
    if (!kimi_login_entry_valid(stored) || !same_login(session, stored)) {
        session->status = kimi_login_entry_valid(stored) ? KIMI_AUTH_CHANGED : KIMI_AUTH_LOGGED_OUT;
        json_decref(stored);
        return -1;
    }
    if (entry_generation(stored) >= entry_generation(session->entry))
        adopt_entry(session, stored);
    json_decref(stored);
    json_int_t expiry = entry_expiry(session->entry);
    if (!allow_refresh)
        return expiry > (json_int_t)time(NULL) + 60 ? 0 : -1;
    if (expiry > (json_int_t)time(NULL) + KIMI_REFRESH_MARGIN_S)
        return 0;
    int result = refresh_session(session, 0, tick, tick_user);
    if (result == 0)
        return 0;
    /* A transient refresh error need not prevent using a still-valid access token. */
    return session->status == KIMI_AUTH_TRANSIENT && expiry > (json_int_t)time(NULL) ? 0 : -1;
}

static char **kimi_auth_headers(const void *auth, const char *session_id, int streaming)
{
    (void)session_id;
    (void)streaming;
    const struct kimi_auth_session *session = auth;
    char **headers = xcalloc(6, sizeof(*headers));
    headers[0] =
        xasprintf("Authorization: Bearer %s", entry_string(session->entry, "access_token"));
    headers[1] = xstrdup("User-Agent: hax/" HAX_VERSION);
    headers[2] = xstrdup("X-Msh-Platform: kimi_cli");
    headers[3] = xstrdup("X-Msh-Version: " HAX_VERSION);
    headers[4] = xasprintf("X-Msh-Device-Id: %s", entry_string(session->entry, "device_id"));
    return headers;
}

static int kimi_auth_recover(void *auth, int *retried, http_tick_cb tick, void *tick_user)
{
    if (*retried)
        return 0;
    *retried = 1;
    return refresh_session(auth, 1, tick, tick_user) == 0;
}

static char *kimi_auth_unauthorized_message(void *auth)
{
    struct kimi_auth_session *session = auth;
    switch (session->status) {
    case KIMI_AUTH_LOGGED_OUT:
        return xstrdup("Kimi is not logged in — run /login kimi");
    case KIMI_AUTH_CHANGED:
        return xstrdup("the Kimi login changed — use /provider to adopt it explicitly");
    case KIMI_AUTH_TRANSIENT:
        return xstrdup("could not refresh the Kimi login — retry, or run /login kimi");
    case KIMI_AUTH_READY:
    case KIMI_AUTH_REJECTED:
        return xstrdup("Kimi login expired or was rejected — run /login kimi again");
    }
    return xstrdup("Kimi authentication failed — run /login kimi");
}

static void kimi_auth_destroy(void *auth)
{
    struct kimi_auth_session *session = auth;
    json_decref(session->entry);
    free(session->login_pin);
    free(session);
}

static const struct http_auth_ops KIMI_AUTH_OPS = {
    .prepare = kimi_auth_prepare,
    .headers = kimi_auth_headers,
    .recover = kimi_auth_recover,
    .unauthorized_message = kimi_auth_unauthorized_message,
    .destroy = kimi_auth_destroy,
};

int kimi_auth_source(const struct provider_def *def, struct http_auth_source *out)
{
    (void)def;
    json_t *entry = cred_store_get("kimi");
    if (!kimi_login_entry_valid(entry)) {
        json_decref(entry);
        hax_err("no Kimi login found — run /login kimi");
        return -1;
    }
    struct kimi_auth_session *session = xcalloc(1, sizeof(*session));
    adopt_entry(session, entry);
    session->login_pin = xstrdup(entry_string(entry, "login_id"));
    json_decref(entry);
    out->ops = &KIMI_AUTH_OPS;
    out->state = session;
    return 0;
}

void kimi_provider_reload_auth(struct provider *provider)
{
    struct kimi_auth_session *session = http_provider_auth(provider)->state;
    json_decref(session->entry);
    session->entry = cred_store_get("kimi");
    free(session->login_pin);
    session->login_pin = xstrdup(entry_string(session->entry, "login_id"));
    session->status =
        kimi_login_entry_valid(session->entry) ? KIMI_AUTH_READY : KIMI_AUTH_LOGGED_OUT;
}

void kimi_prepare_availability(const struct provider_def *def, struct provider_availability *out)
{
    (void)def;
    out->available = kimi_login_present();
    out->reason = out->available ? NULL : xstrdup("not logged in (use /login kimi)");
}

void kimi_parse_model(const json_t *entry, struct model_info *out)
{
    json_t *context = json_object_get(entry, "context_length");
    if (json_is_integer(context) && json_integer_value(context) > 0)
        out->context = (long)json_integer_value(context);
    json_t *image = json_object_get(entry, "supports_image_in");
    if (json_is_boolean(image))
        out->image_input = json_is_true(image) ? PROVIDER_CAP_YES : PROVIDER_CAP_NO;
    const char *name = entry_string(entry, "display_name");
    if (name && *name)
        out->description = xstrdup(name);
}
