/* SPDX-License-Identifier: MIT */
#include "providers/vertex.h"

#include <ctype.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "diag.h"
#include "trace.h"
#include "xalloc.h"
#include "providers/http_provider.h"
#include "system/fs.h"
#include "system/path.h"
#include "system/spawn.h"
#include "text/url.h"
#include "transport/http.h"

/* Vertex's Anthropic endpoint serves the Messages wire under a URL that carries the project,
 * location, and model; see registry.c's def for the path template. Authentication is a Google
 * access token sourced from user credentials — never a static API key. */

#define VERTEX_OAUTH_URL "https://oauth2.googleapis.com/token"
/* Instrumentation seam for tests: overrides the token endpoint (a fixed public URL would make
 * the refresh flow untestable offline). */
#define VERTEX_OAUTH_URL_ENV   "HAX_VERTEX_OAUTH_URL"
#define VERTEX_TOKEN_MAX_BYTES (64 * 1024)
#define VERTEX_TOKEN_TIMEOUT_S 10
/* Refresh slightly early so an access token does not expire mid-request. */
#define VERTEX_TOKEN_MARGIN_S 60
/* Hobbling a SM service account needs no token; gcloud's print-access-token is fast, but a
 * workload-identity federation call can take a moment, so bound it. */
#define VERTEX_GCLOUD_TIMEOUT_MS 15000
#define VERTEX_GCLOUD_MAX_BYTES  8192

/* The default region mirrors Claude Code's CLOUD_ML_REGION default; users override it per
 * deployment. Host and path both consume resolve_settings() so their fallbacks cannot diverge. */
static const char *env_nonempty(const char *name)
{
    const char *value = getenv(name);
    return value && *value ? value : NULL;
}

struct vertex_settings {
    const char *project;
    const char *location;
};

static const char *resolve_setting(const char *key, const char *primary_env,
                                   const char *alternate_env, const char *fallback)
{
    const char *value = config_str_nonempty(key);
    if (value)
        return value;
    value = env_nonempty(primary_env);
    if (value)
        return value;
    value = alternate_env ? env_nonempty(alternate_env) : NULL;
    return value ? value : fallback;
}

static struct vertex_settings resolve_settings(void)
{
    return (struct vertex_settings){
        .project = resolve_setting("providers.vertex.project", "GOOGLE_CLOUD_PROJECT",
                                   "ANTHROPIC_VERTEX_PROJECT_ID", NULL),
        .location = resolve_setting("providers.vertex.location", "GOOGLE_CLOUD_LOCATION",
                                    "CLOUD_ML_REGION", "us-east5"),
    };
}

static const char *config_literal_token(void)
{
    return resolve_setting("providers.vertex.access_token", "GOOGLE_OAUTH_ACCESS_TOKEN", NULL,
                           NULL);
}

static int project_valid(const char *project)
{
    size_t len = strlen(project);
    if (len == 0 || len > 128 || !isalnum((unsigned char)project[0]) ||
        !isalnum((unsigned char)project[len - 1]))
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)project[i];
        if (!islower(c) && !isdigit(c) && c != '-' && c != '.' && c != ':')
            return 0;
    }
    return 1;
}

static int location_valid(const char *location)
{
    size_t len = strlen(location);
    if (len == 0 || len > 63 || !isalnum((unsigned char)location[0]) ||
        !isalnum((unsigned char)location[len - 1]))
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)location[i];
        if (!islower(c) && !isdigit(c) && c != '-')
            return 0;
    }
    return 1;
}

static int validate_settings(const struct vertex_settings *settings)
{
    if (!settings->project) {
        hax_err("provider 'vertex': project is not set (configure providers.vertex.project / "
                "HAX_VERTEX_PROJECT, GOOGLE_CLOUD_PROJECT, or ANTHROPIC_VERTEX_PROJECT_ID)");
        return -1;
    }
    if (!project_valid(settings->project)) {
        hax_err("provider 'vertex': invalid project (use a Google Cloud project ID or number)");
        return -1;
    }
    if (!location_valid(settings->location)) {
        hax_err("provider 'vertex': invalid location (use lowercase letters, digits, and hyphens)");
        return -1;
    }
    return 0;
}

char *vertex_resolve_base_url(const struct provider_def *def)
{
    (void)def;
    struct vertex_settings settings = resolve_settings();
    if (validate_settings(&settings) != 0)
        return NULL;

    char *host;
    if (strcmp(settings.location, "global") == 0) {
        host = xstrdup("aiplatform.googleapis.com");
    } else if (strcmp(settings.location, "us") == 0 || strcmp(settings.location, "eu") == 0) {
        /* Multi-region endpoints are served from the region's replica host. */
        host = xasprintf("aiplatform.%s.rep.googleapis.com", settings.location);
    } else {
        host = xasprintf("%s-aiplatform.googleapis.com", settings.location);
    }
    char *url = xasprintf("https://%s", host);
    free(host);
    return url;
}

char *vertex_resolve_path(const struct provider_def *def)
{
    (void)def;
    struct vertex_settings settings = resolve_settings();
    if (validate_settings(&settings) != 0)
        return NULL;
    return xasprintf("/v1/projects/%s/locations/%s/publishers/anthropic/models/"
                     "{model}:streamRawPredict",
                     settings.project, settings.location);
}

/* ---- credentials ----
 *
 * A session holds one access token plus the recipe to renew it. Resolution order mirrors what
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

/* Default ADC path, or the configured GOOGLE_APPLICATION_CREDENTIALS. The caller must not free
 * the result if it aliases the env. */
static const char *adc_path(void)
{
    const char *configured = env_nonempty("GOOGLE_APPLICATION_CREDENTIALS");
    static char *cached;
    if (configured)
        return configured;
    if (!cached)
        cached = path_expand_home("~/.config/gcloud/application_default_credentials.json");
    return cached;
}

/* Read and parse the ADC file, or NULL when missing/invalid (caller frees with json_decref). */
static json_t *load_adc(void)
{
    const char *path = adc_path();
    if (!path || !*path)
        return NULL;
    char *contents = fs_read_file(path, NULL);
    if (!contents)
        return NULL;
    json_t *root = json_loads(contents, 0, NULL);
    free(contents);
    return json_is_object(root) ? root : NULL;
}

/* Exchange the ADC file's own client_id/client_secret and refresh token for a new access token. */
static int refresh_user_token(struct vertex_auth *a, char **detail)
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
    int rc =
        http_post(token_url, NULL, "application/x-www-form-urlencoded", body, strlen(body),
                  VERTEX_TOKEN_TIMEOUT_S, VERTEX_TOKEN_MAX_BYTES, NULL, NULL, &response, &status);
    free(body);
    if (rc != 0 || status != 200 || !response) {
        if (detail)
            *detail = xstrdup("OAuth token refresh failed");
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
    a->token_expires = ttl > 0 ? time(NULL) + ttl : 0;
    json_decref(root);
    return 0;
}

/* Have gcloud print an access token for whatever identity the ADC file describes (service
 * account, workload identity federation, or impersonation). */
static int refresh_service_token(struct vertex_auth *a, char **detail)
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
    char *output =
        spawn_capture_stdout(argv, VERTEX_GCLOUD_MAX_BYTES, VERTEX_GCLOUD_TIMEOUT_MS, &length);
    free(gcloud);
    if (!output) {
        if (detail)
            *detail = xstrdup("`gcloud auth application-default print-access-token` failed");
        return -1;
    }
    /* Strip the trailing newline gcloud emits. */
    while (length > 0 && isspace((unsigned char)output[length - 1]))
        output[--length] = '\0';
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

    json_t *root = load_adc();
    if (!root) {
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

/* Bring a->token fresh. `force` skips the soft-expiry check (a rejected request). */
static int verify_token(struct vertex_auth *a, int force)
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
    /* Re-read the source before renewing so an edited ADC file takes effect. Losing the source is
     * terminal (the user changed something mid-session). */
    if (load_credentials(a) != 0)
        return -1;
    char *detail = NULL;
    int rc =
        a->kind == VERTEX_USER ? refresh_user_token(a, &detail) : refresh_service_token(a, &detail);
    if (rc != 0) {
        set_fatal(a, detail ? detail : "Google credential refresh failed");
        free(detail);
        return -1;
    }
    return 0;
}

static int vertex_auth_prepare(void *auth, int allow_refresh, http_tick_cb tick, void *tick_user)
{
    (void)tick;
    (void)tick_user;
    struct vertex_auth *a = auth;
    if (!a->token) {
        if (load_credentials(a) != 0)
            return -1;
        return verify_token(a, 0) == 0 ? 0 : -1;
    }
    if (a->kind != VERTEX_LITERAL && a->token_expires > 0 && allow_refresh &&
        time(NULL) >= a->token_expires)
        return verify_token(a, 1) == 0 ? 0 : -1;
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

/* One forced renewal after a 401. */
static int vertex_auth_recover(void *auth, int *retried, http_tick_cb tick, void *tick_user)
{
    (void)tick;
    (void)tick_user;
    struct vertex_auth *a = auth;
    if (*retried)
        return 0;
    *retried = 1;
    if (verify_token(a, 1) == 0)
        return 1;
    free(a->token);
    a->token = NULL;
    return 0;
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

void vertex_prepare_availability(const struct provider_def *def, struct provider_availability *out)
{
    (void)def;
    memset(out, 0, sizeof(*out));
    if (!resolve_settings().project) {
        out->available = 0;
        out->reason = xstrdup("project not set");
        return;
    }
    struct vertex_auth a = {0};
    if (load_credentials(&a) != 0) {
        out->available = 0;
        out->reason = xstrdup("ADC unavailable");
    } else if (a.kind == VERTEX_SERVICE) {
        char *gcloud = fs_which("gcloud");
        out->available = gcloud != NULL;
        if (!gcloud)
            out->reason = xstrdup("gcloud not found");
        free(gcloud);
    } else {
        out->available = 1;
    }
    vertex_auth_clear(&a);
}
