/* SPDX-License-Identifier: MIT */
#include "providers/vertex_auth.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "util.h"
#include "system/path.h"
#include "transport/http.h"

#define VERTEX_OAUTH_URL "https://oauth2.googleapis.com/token"
/* Test/instrumentation seam: overrides the token endpoint (a fixed public URL would make the
 * full refresh flow untestable offline). */
#define VERTEX_OAUTH_URL_ENV   "HAX_VERTEX_OAUTH_URL"
#define VERTEX_TOKEN_MAX_BYTES (64 * 1024)
#define VERTEX_TOKEN_TIMEOUT_S 10
/* Refresh slightly early so an access token does not expire mid-request. */
#define VERTEX_TOKEN_MARGIN_S 60
/* gcloud's public OAuth client uses this well-known secret for user ADC. */
#define VERTEX_GCLOUD_CLIENT_SECRET "gdf"

static char *cached_token;
static time_t cached_resolved_at;
static long cached_expires_in;

static const char *env_nonempty(const char *name)
{
    const char *value = getenv(name);
    return value && *value ? value : NULL;
}

/* Percent-encode one byte for a form body. */
static char *form_encode(const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    struct buf out;
    buf_init(&out);
    for (const char *cursor = value; *cursor; cursor++) {
        unsigned char byte = (unsigned char)*cursor;
        if (('A' <= byte && byte <= 'Z') || ('a' <= byte && byte <= 'z') ||
            ('0' <= byte && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
            byte == '~') {
            buf_append(&out, cursor, 1);
        } else {
            char esc[3] = {'%', hex[byte >> 4], hex[byte & 0xF]};
            buf_append(&out, esc, sizeof(esc));
        }
    }
    return buf_steal(&out);
}

static char *default_adc_path(void)
{
    return path_expand_home("~/.config/gcloud/application_default_credentials.json");
}

/* Peek at the ADC file to classify its kind: "user" (refresh flow) or "service" (needs JWT).
 * Returns NULL when no usable ADC file is present. */
static json_t *load_adc_file(void)
{
    const char *configured = env_nonempty("GOOGLE_APPLICATION_CREDENTIALS");
    char *path = configured ? xstrdup(configured) : default_adc_path();
    if (!path)
        return NULL;

    size_t length = 0;
    char *contents = slurp_file(path, &length);
    free(path);
    if (!contents)
        return NULL;

    json_t *root = json_loads(contents, 0, NULL);
    free(contents);
    if (!json_is_object(root)) {
        json_decref(root);
        return NULL;
    }
    return root;
}

int vertex_auth_available(const char **reason)
{
    if (reason)
        *reason = NULL;

    const char *literal = config_str("vertex.access_token");
    if (!literal || !*literal)
        literal = env_nonempty("GOOGLE_OAUTH_ACCESS_TOKEN");
    if (literal)
        return 1;

    json_t *root = load_adc_file();
    if (!root) {
        if (reason)
            *reason = "no Google ADC credentials (run `gcloud auth application-default login` "
                      "or set GOOGLE_OAUTH_ACCESS_TOKEN)";
        return 0;
    }
    int service_account = json_object_get(root, "private_key") != NULL;
    int has_refresh = json_string_value(json_object_get(root, "refresh_token")) != NULL;
    json_decref(root);
    if (service_account) {
        if (reason)
            *reason = "service-account ADC is unsupported — run `gcloud auth "
                      "application-default login` for a user credential";
        return 0;
    }
    if (!has_refresh) {
        if (reason)
            *reason = "Google ADC file has no refresh_token";
        return 0;
    }
    return 1;
}

/* Exchange the refresh token for a short-lived access token. */
static int refresh_access_token(const char *client_id, const char *client_secret,
                                const char *refresh_token)
{
    char *encoded_id = form_encode(client_id);
    char *encoded_secret = form_encode(client_secret);
    char *encoded_refresh = form_encode(refresh_token);
    char *body = xasprintf("grant_type=refresh_token&client_id=%s&client_secret=%s"
                           "&refresh_token=%s",
                           encoded_id, encoded_secret, encoded_refresh);
    free(encoded_id);
    free(encoded_secret);
    free(encoded_refresh);

    const char *configured_url = env_nonempty(VERTEX_OAUTH_URL_ENV);
    const char *url = configured_url ? configured_url : VERTEX_OAUTH_URL;
    char *response = NULL;
    long status = 0;
    int result =
        http_post(url, NULL, "application/x-www-form-urlencoded", body, strlen(body),
                  VERTEX_TOKEN_TIMEOUT_S, VERTEX_TOKEN_MAX_BYTES, NULL, NULL, &response, &status);
    free(body);
    if (result != 0 || !response)
        return -1;

    json_error_t json_error;
    json_t *root = json_loads(response, 0, &json_error);
    free(response);
    if (!json_is_object(root))
        return -1;

    const char *token = json_string_value(json_object_get(root, "access_token"));
    if (!token || !*token) {
        json_decref(root);
        return -1;
    }

    free(cached_token);
    cached_token = xstrdup(token);
    cached_resolved_at = time(NULL);
    cached_expires_in = 0;
    json_t *expires = json_object_get(root, "expires_in");
    if (json_is_integer(expires) && json_integer_value(expires) > 0)
        cached_expires_in = (long)json_integer_value(expires);
    json_decref(root);
    return 0;
}

void vertex_auth_invalidate(void)
{
    free(cached_token);
    cached_token = NULL;
    cached_resolved_at = 0;
    cached_expires_in = 0;
}

const char *vertex_auth_token(char **error)
{
    if (cached_token && cached_resolved_at > 0 && cached_expires_in > 0 &&
        time(NULL) < cached_resolved_at + cached_expires_in - VERTEX_TOKEN_MARGIN_S)
        return cached_token;

    /* A literal token short-circuits file discovery (service-account workflows). It has no
     * soft expiry; a rejected request calls vertex_auth_invalidate to re-read it. */
    const char *literal = config_str("vertex.access_token");
    if (!literal || !*literal)
        literal = env_nonempty("GOOGLE_OAUTH_ACCESS_TOKEN");
    if (literal) {
        free(cached_token);
        cached_token = xstrdup(literal);
        cached_resolved_at = time(NULL);
        cached_expires_in = 0;
        return cached_token;
    }

    json_t *root = load_adc_file();
    if (!root) {
        if (error)
            *error = xstrdup("no Google ADC credentials (run `gcloud auth application-default "
                             "login` or set GOOGLE_OAUTH_ACCESS_TOKEN)");
        return NULL;
    }
    if (json_object_get(root, "private_key")) {
        if (error)
            *error = xstrdup("service-account ADC is unsupported by hax (it needs a signed "
                             "JWT); run `gcloud auth application-default login` or set "
                             "GOOGLE_OAUTH_ACCESS_TOKEN to a manual access token");
        json_decref(root);
        return NULL;
    }

    const char *refresh_json = json_string_value(json_object_get(root, "refresh_token"));
    const char *id_json = json_string_value(json_object_get(root, "client_id"));
    const char *secret_json = json_string_value(json_object_get(root, "client_secret"));

    char *refresh_token = refresh_json && *refresh_json ? xstrdup(refresh_json) : NULL;
    char *client_id = id_json && *id_json ? xstrdup(id_json) : NULL;
    char *client_secret = secret_json && *secret_json ? xstrdup(secret_json) : NULL;
    json_decref(root);

    if (!client_secret) {
        const char *env_secret = env_nonempty("GOOGLE_OAUTH_CLIENT_SECRET");
        if (env_secret)
            client_secret = xstrdup(env_secret);
        else
            client_secret = xstrdup(VERTEX_GCLOUD_CLIENT_SECRET);
    }

    if (!refresh_token || !client_id) {
        if (error)
            *error = xstrdup("Google ADC file has no usable refresh_token/client_id");
        free(refresh_token);
        free(client_id);
        free(client_secret);
        return NULL;
    }

    int rc = refresh_access_token(client_id, client_secret, refresh_token);
    free(refresh_token);
    free(client_id);
    free(client_secret);
    if (rc != 0) {
        if (error)
            *error = xstrdup("OAuth token refresh failed");
        return NULL;
    }
    return cached_token;
}
