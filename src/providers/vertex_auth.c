/* SPDX-License-Identifier: MIT */
#include "providers/vertex_auth.h"

#include <ctype.h>
#include <errno.h>
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "trace.h"
#include "xalloc.h"
#include "providers/http_provider.h"
#include "system/fs.h"
#include "system/path.h"
#include "system/spawn.h"
#include "text/url.h"
#include "transport/http.h"

#define VERTEX_OAUTH_URL "https://oauth2.googleapis.com/token"
/* Instrumentation seam for tests: overrides the token endpoint (a fixed public URL would make
 * the refresh flow untestable offline). */
#define VERTEX_OAUTH_URL_ENV   "HAX_VERTEX_OAUTH_URL"
#define VERTEX_TOKEN_MAX_BYTES (64 * 1024)
#define VERTEX_TOKEN_TIMEOUT_S 10
/* Refresh slightly early so an access token does not expire mid-request. */
#define VERTEX_TOKEN_MARGIN_S 60
/* gcloud's print-access-token is fast, but a workload-identity federation call can take a
 * moment, so bound it. */
#define VERTEX_GCLOUD_TIMEOUT_MS 15000
#define VERTEX_GCLOUD_MAX_BYTES  8192

/* A session holds one access token plus the recipe to renew it. Resolution order mirrors what
 * users already have configured: an explicit token (no refresh), the ADC file's authorized_user
 * shape (refresh-token exchange), or any other ADC kind (shell out to gcloud — covering service
 * accounts, workload identity federation, and impersonation without hand-rolling RS256). */

enum vertex_auth_kind {
    VERTEX_LITERAL, /* explicit token; rejected by 401 → re-read the literal once */
    VERTEX_USER,    /* ADC authorized_user: OAuth refresh-token exchange */
    VERTEX_SERVICE, /* other ADC kinds: `gcloud auth application-default print-access-token` */
};

struct vertex_auth {
    enum vertex_auth_kind kind;
    char *token;
    time_t token_expires; /* 0 = no soft expiry */
    /* user flow, read from the ADC file on each refresh so edits take effect */
    char *client_id;
    char *client_secret;
    char *refresh_token;
    /* Opened as logged-out with a blocking reason to report on the next request. */
    char *fatal;
};

static const char *env_nonempty(const char *name)
{
    const char *value = getenv(name);
    return value && *value ? value : NULL;
}

static const char *config_literal_token(void)
{
    const char *token = config_str_nonempty("providers.vertex.access_token");
    return token ? token : env_nonempty("GOOGLE_OAUTH_ACCESS_TOKEN");
}

/* Owned ADC path: GOOGLE_APPLICATION_CREDENTIALS when set, else CLOUDSDK_CONFIG's directory,
 * else the default under ~/.config/gcloud. */
static char *adc_path(void)
{
    const char *configured = env_nonempty("GOOGLE_APPLICATION_CREDENTIALS");
    if (configured)
        return xstrdup(configured);
    const char *config_dir = env_nonempty("CLOUDSDK_CONFIG");
    if (config_dir)
        return path_join(config_dir, "application_default_credentials.json");
    static char *cached;
    if (!cached)
        cached = path_expand_home("~/.config/gcloud/application_default_credentials.json");
    return cached ? xstrdup(cached) : NULL;
}

/* Why an ADC file did not yield a JSON object. */
enum adc_load_error {
    ADC_LOAD_ABSENT,     /* no file at the resolved path */
    ADC_LOAD_UNREADABLE, /* a file exists but could not be opened */
    ADC_LOAD_MALFORMED,  /* not valid JSON, or not a JSON object */
};

/* Read and parse the ADC file, reporting why a NULL root is returned. The caller frees a
 * non-NULL result with json_decref. */
static json_t *load_adc(enum adc_load_error *error)
{
    *error = ADC_LOAD_ABSENT;
    char *path = adc_path();
    if (!path)
        return NULL;
    char *contents = fs_read_file(path, NULL);
    int read_errno = errno;
    free(path);
    if (!contents) {
        *error = read_errno == ENOENT ? ADC_LOAD_ABSENT : ADC_LOAD_UNREADABLE;
        return NULL;
    }
    json_t *root = json_loads(contents, 0, NULL);
    free(contents);
    if (!json_is_object(root)) {
        json_decref(root);
        *error = ADC_LOAD_MALFORMED;
        return NULL;
    }
    return root;
}

/* Whether an OAuth error response names `code`, e.g. invalid_grant. */
static int oauth_error_is(const char *response, const char *code)
{
    if (!response)
        return 0;
    json_t *root = json_loads(response, 0, NULL);
    const char *error =
        json_is_object(root) ? json_string_value(json_object_get(root, "error")) : NULL;
    int match = error && strcmp(error, code) == 0;
    json_decref(root);
    return match;
}

/* Exchange the ADC file's own client_id/client_secret and refresh token for a new access token. */
static int refresh_user_token(struct vertex_auth *a, char **detail, http_tick_cb tick,
                              void *tick_user)
{
    char *encoded_id = url_encode(a->client_id);
    char *encoded_secret = url_encode(a->client_secret);
    char *encoded_refresh = url_encode(a->refresh_token);
    /* http_post traces the form and error response before returning. */
    trace_register_secret(a->client_secret);
    trace_register_secret(encoded_secret);
    trace_register_secret(a->refresh_token);
    trace_register_secret(encoded_refresh);
    char *body = xasprintf("grant_type=refresh_token&client_id=%s&client_secret=%s"
                           "&refresh_token=%s",
                           encoded_id, encoded_secret, encoded_refresh);
    free(encoded_id);
    free(encoded_secret);
    free(encoded_refresh);

    const char *url = env_nonempty(VERTEX_OAUTH_URL_ENV);
    const char *token_url = url ? url : VERTEX_OAUTH_URL;
    char *response = NULL;
    long status = 0;
    int rc = http_post(token_url, NULL, "application/x-www-form-urlencoded", body, strlen(body),
                       VERTEX_TOKEN_TIMEOUT_S, VERTEX_TOKEN_MAX_BYTES, tick, tick_user, &response,
                       &status);
    free(body);
    if (rc != 0) {
        if (detail)
            *detail = xstrdup("OAuth token refresh failed");
        free(response);
        return -1;
    }
    if (status != 200 || !response) {
        /* A rejected refresh token is a re-authentication problem, not a transient failure. */
        if (detail) {
            if (oauth_error_is(response, "invalid_grant"))
                *detail = xstrdup("OAuth token refresh rejected (invalid_grant) — run `gcloud "
                                  "auth application-default login` to re-authenticate");
            else
                *detail = xasprintf("OAuth token refresh failed (HTTP %ld)", status);
        }
        free(response);
        return -1;
    }
    json_error_t json_error = {0};
    json_t *root = json_loads(response, 0, &json_error);
    free(response);
    const char *token =
        json_is_object(root) ? json_string_value(json_object_get(root, "access_token")) : NULL;
    if (!token || !*token) {
        json_decref(root);
        if (detail)
            *detail = xstrdup("OAuth token refresh returned no access_token");
        return -1;
    }
    free(a->token);
    a->token = xstrdup(token);
    trace_register_secret(a->token);
    json_t *expires = json_object_get(root, "expires_in");
    long ttl = json_is_number(expires) ? (long)json_integer_value(expires) : 0;
    /* Early-refresh margin, applied to user tokens too: a token not outliving the margin is
     * treated as already expiring. */
    a->token_expires = ttl > VERTEX_TOKEN_MARGIN_S ? time(NULL) + ttl - VERTEX_TOKEN_MARGIN_S
                                                   : (ttl > 0 ? time(NULL) : 0);
    json_decref(root);
    return 0;
}

/* Have gcloud print an access token for whatever identity the ADC file describes (service
 * account, workload identity federation, or impersonation). Polls `tick` so a cancelled caller
 * aborts without waiting out the timeout, killing and reaping the child. */
static int refresh_service_token(struct vertex_auth *a, char **detail, http_tick_cb tick,
                                 void *tick_user)
{
    char *gcloud = fs_which("gcloud");
    if (!gcloud) {
        if (detail)
            *detail = xstrdup("gcloud not found — install the Google Cloud CLI and put gcloud on "
                              "PATH, or set GOOGLE_OAUTH_ACCESS_TOKEN");
        return -1;
    }
    const char *const argv[] = {gcloud, "auth", "application-default", "print-access-token", NULL};
    size_t length = 0;
    char *output = spawn_capture_stdout_checked(argv, VERTEX_GCLOUD_MAX_BYTES,
                                                VERTEX_GCLOUD_TIMEOUT_MS, tick, tick_user, &length);
    free(gcloud);
    if (!output) {
        if (detail)
            *detail = xstrdup("`gcloud auth application-default print-access-token` failed");
        return -1;
    }
    /* gcloud emits a trailing newline; trim surrounding whitespace and reject a blank token. */
    size_t start = 0;
    while (start < length && isspace((unsigned char)output[start]))
        start++;
    while (length > start && isspace((unsigned char)output[length - 1]))
        length--;
    if (start >= length) {
        if (detail)
            *detail = xstrdup("`gcloud auth application-default print-access-token` returned no "
                              "usable token");
        free(output);
        return -1;
    }
    if (start > 0)
        memmove(output, output + start, length - start);
    output[length - start] = '\0';
    free(a->token);
    a->token = xstrdup(output);
    trace_register_secret(a->token);
    /* gcloud tokens are short-lived (~1h); treat them as expiring so a request refreshes rather
     * than letting one die mid-turn. */
    a->token_expires = time(NULL) + 3600 - VERTEX_TOKEN_MARGIN_S;
    free(output);
    return 0;
}

static void set_fatal(struct vertex_auth *a, const char *message)
{
    free(a->fatal);
    a->fatal = xstrdup(message);
}

/* Classify what the current ADC file (or literal token) offers and load its long-lived secret
 * material, leaving the short-lived token to a refresh. Reports none usable via `fatal`. */
static int load_credentials(struct vertex_auth *a)
{
    free(a->fatal);
    a->fatal = NULL;
    const char *literal = config_literal_token();
    if (literal) {
        a->kind = VERTEX_LITERAL;
        free(a->token);
        a->token = xstrdup(literal);
        trace_register_secret(a->token);
        a->token_expires = 0;
        return 0;
    }

    enum adc_load_error error;
    json_t *root = load_adc(&error);
    if (!root) {
        if (error == ADC_LOAD_UNREADABLE)
            set_fatal(a, "Google ADC file is unreadable — check permissions on "
                         "GOOGLE_APPLICATION_CREDENTIALS");
        else if (error == ADC_LOAD_MALFORMED)
            set_fatal(a, "Google ADC file is not valid JSON — rerun `gcloud auth "
                         "application-default login`");
        else
            set_fatal(a, "no Google ADC credentials (run `gcloud auth application-default login`, "
                         "or set GOOGLE_OAUTH_ACCESS_TOKEN)");
        return -1;
    }
    const char *type = json_string_value(json_object_get(root, "type"));
    const char *refresh = json_string_value(json_object_get(root, "refresh_token"));
    if (type && strcmp(type, "authorized_user") == 0 && refresh && *refresh) {
        const char *client_id = json_string_value(json_object_get(root, "client_id"));
        const char *client_secret = json_string_value(json_object_get(root, "client_secret"));
        if (client_id && *client_id && client_secret && *client_secret) {
            a->kind = VERTEX_USER;
            free(a->client_id);
            free(a->client_secret);
            free(a->refresh_token);
            a->client_id = xstrdup(client_id);
            a->client_secret = xstrdup(client_secret);
            a->refresh_token = xstrdup(refresh);
            json_decref(root);
            return 0;
        }
        set_fatal(a, "Google ADC file has no usable refresh_token/client_id — rerun `gcloud "
                     "auth application-default login`");
        json_decref(root);
        return -1;
    }
    json_decref(root);
    /* Service-account and every other ADC shape: let gcloud print a token. */
    a->kind = VERTEX_SERVICE;
    return 0;
}

/* Bring a->token fresh. `force` skips the soft-expiry check (a rejected request). A renewable
 * source refreshes only when `allow_refresh` permits HTTP/process execution. */
static int verify_token(struct vertex_auth *a, int force, int allow_refresh, http_tick_cb tick,
                        void *tick_user)
{
    if (!a->token && a->fatal)
        return -1;
    if (!force && a->token && (a->token_expires == 0 || time(NULL) < a->token_expires))
        return 0;

    if (a->kind == VERTEX_LITERAL) {
        /* A literal token has no soft expiry; only a 401 forces a re-read here. */
        if (force)
            return load_credentials(a);
        return a->token ? 0 : -1;
    }
    if (!allow_refresh)
        return -1;
    /* Re-read the source before renewing so an edited ADC file (or a new literal) takes effect.
     * Losing the source is terminal (the user changed something mid-session). */
    if (load_credentials(a) != 0)
        return -1;
    if (a->kind == VERTEX_LITERAL) /* the source changed to a literal; nothing to exchange */
        return 0;
    char *detail = NULL;
    int rc = a->kind == VERTEX_USER ? refresh_user_token(a, &detail, tick, tick_user)
                                    : refresh_service_token(a, &detail, tick, tick_user);
    if (rc != 0) {
        set_fatal(a, detail ? detail : "Google credential refresh failed");
        free(detail);
        return -1;
    }
    return 0;
}

static int vertex_auth_prepare(void *auth, int allow_refresh, http_tick_cb tick, void *tick_user)
{
    struct vertex_auth *a = auth;
    if (!a->token) {
        /* Local reload only: classify what the source offers now. */
        if (load_credentials(a) != 0)
            return -1;
        if (a->kind == VERTEX_LITERAL)
            return 0;
        if (!allow_refresh) /* acquiring a renewable token needs a refresh, which is forbidden */
            return -1;
        return verify_token(a, 0, allow_refresh, tick, tick_user) == 0 ? 0 : -1;
    }
    int expired = a->token_expires > 0 && time(NULL) >= a->token_expires;
    if (!allow_refresh)
        return expired ? -1 : 0;
    if (a->kind != VERTEX_LITERAL && expired)
        return verify_token(a, 1, allow_refresh, tick, tick_user) == 0 ? 0 : -1;
    return 0;
}

static char **vertex_auth_headers(const void *auth, const char *session_id, int streaming)
{
    (void)session_id;
    (void)streaming;
    const struct vertex_auth *a = auth;
    char *authorization = xasprintf("Authorization: Bearer %s", a->token ? a->token : "");
    const char *fixed[] = {authorization, NULL};
    char **headers = string_array_concat(fixed, NULL);
    free(authorization);
    return headers;
}

/* One forced recovery after a 401. A renewable source retries after a forced renewal; a rejected
 * literal is reused only when re-reading it produced a different token. */
static int vertex_auth_recover(void *auth, int *retried, http_tick_cb tick, void *tick_user)
{
    struct vertex_auth *a = auth;
    if (*retried)
        return 0;
    *retried = 1;

    enum vertex_auth_kind previous_kind = a->kind;
    char *old_token = a->token ? xstrdup(a->token) : NULL;
    if (verify_token(a, 1, 1, tick, tick_user) != 0) {
        free(old_token);
        free(a->token);
        a->token = NULL;
        return 0;
    }
    int token_changed = !old_token || strcmp(old_token, a->token) != 0;
    free(old_token);
    return previous_kind != VERTEX_LITERAL || token_changed;
}

static char *vertex_auth_unauthorized_message(void *auth)
{
    struct vertex_auth *a = auth;
    if (!a->token && a->fatal)
        return xstrdup(a->fatal);
    return xstrdup("Google rejected the access token — run `gcloud auth application-default "
                   "login`, or check GOOGLE_OAUTH_ACCESS_TOKEN");
}

static void vertex_auth_clear(struct vertex_auth *a)
{
    free(a->token);
    free(a->client_id);
    free(a->client_secret);
    free(a->refresh_token);
    free(a->fatal);
}

static void vertex_auth_destroy(void *auth)
{
    struct vertex_auth *a = auth;
    vertex_auth_clear(a);
    free(a);
}

static const struct http_auth_ops VERTEX_AUTH_OPS = {
    .prepare = vertex_auth_prepare,
    .headers = vertex_auth_headers,
    .recover = vertex_auth_recover,
    .unauthorized_message = vertex_auth_unauthorized_message,
    .destroy = vertex_auth_destroy,
};

int vertex_auth_source(const struct provider_def *def, struct http_auth_source *out)
{
    (void)def;
    struct vertex_auth *a = xcalloc(1, sizeof(*a));
    /* A missing credential source does not fail construction: the session reports setup steps
     * on the first request. */
    load_credentials(a);
    out->ops = &VERTEX_AUTH_OPS;
    out->state = a;
    return 0;
}

int vertex_auth_local_status(char **reason)
{
    *reason = NULL;
    if (config_literal_token())
        return 1;

    enum adc_load_error error;
    json_t *root = load_adc(&error);
    if (!root) {
        *reason = xstrdup("ADC unavailable");
        return 0;
    }
    const char *type = json_string_value(json_object_get(root, "type"));
    const char *refresh = json_string_value(json_object_get(root, "refresh_token"));
    int usable = 1;
    if (type && strcmp(type, "authorized_user") == 0 && refresh && *refresh) {
        const char *client_id = json_string_value(json_object_get(root, "client_id"));
        const char *client_secret = json_string_value(json_object_get(root, "client_secret"));
        usable = client_id && *client_id && client_secret && *client_secret;
    }
    if (!usable) {
        json_decref(root);
        *reason = xstrdup("ADC unavailable");
        return 0;
    }
    /* Only delegated kinds need the CLI; nothing external has run yet. */
    int needs_gcloud = !(type && strcmp(type, "authorized_user") == 0);
    json_decref(root);
    if (!needs_gcloud)
        return 1;
    char *gcloud = fs_which("gcloud");
    if (!gcloud) {
        *reason = xstrdup("gcloud not found");
        return 0;
    }
    free(gcloud);
    return 1;
}