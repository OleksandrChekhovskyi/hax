/* SPDX-License-Identifier: MIT */
#include "providers/vertex.h"

#include <curl/curl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "providers/http_provider.h"
#include "providers/registry.h"
#include "diag.h"
#include "provider.h"
#include "provider_config.h"
#include "xalloc.h"
#include "text/url.h"

struct vertex_auth_state {
    int static_token;
    char *token;
    time_t expiry;

    char *client_id;
    char *client_secret;
    char *refresh_token;

    char *adc_type;
};

struct vertex_buf {
    char *data;
    size_t len;
    size_t cap;
};

static size_t vertex_write_cb(char *ptr, size_t size, size_t nmemb, void *user)
{
    struct vertex_buf *buf = user;
    size_t incoming = size * nmemb;
    if (buf->len + incoming + 1 > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap * 2 : 4096;
        while (new_cap < buf->len + incoming + 1)
            new_cap *= 2;
        buf->data = xrealloc(buf->data, new_cap);
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, incoming);
    buf->len += incoming;
    buf->data[buf->len] = '\0';
    return incoming;
}

/* A setting of this provider, by its canonical key: the registry resolves the environment
 * variable it carries and the value in config.json. */
static char *vertex_setting(const char *key)
{
    const char *value = config_scoped_str("providers.vertex-ai", key);
    return value && *value ? xstrdup(value) : NULL;
}

/* The project the calls bill and belong to. */
static char *vertex_project_id(void)
{
    /* The registry does not carry the project yet, so its names are read here. */
    const char *alias = getenv("HAX_VERTEX_PROJECT");
    if (alias && *alias)
        return xstrdup(alias);
    char *project = vertex_setting("project");
    if (!project) {
        const char *fallbacks[] = {"VERTEXAI_PROJECT", "GOOGLE_CLOUD_PROJECT", "GCLOUD_PROJECT",
                                   "GOOGLE_PROJECT_ID", NULL};
        for (size_t i = 0; fallbacks[i]; i++) {
            const char *value = getenv(fallbacks[i]);
            if (value && *value) {
                project = xstrdup(value);
                break;
            }
        }
    }
    return project;
}

/* The configured location, defaulted to the one Google defaults to. */
static char *vertex_location(void)
{
    /* The registry does not carry the location yet, so its names are read here. */
    const char *alias = getenv("HAX_VERTEX_LOCATION");
    if (alias && *alias)
        return xstrdup(alias);
    char *location = vertex_setting("location");
    if (!location) {
        const char *fallbacks[] = {"VERTEXAI_LOCATION", "GOOGLE_CLOUD_REGION",
                                   "CLOUDSDK_COMPUTE_REGION", NULL};
        for (size_t i = 0; fallbacks[i]; i++) {
            const char *value = getenv(fallbacks[i]);
            if (value && *value) {
                location = xstrdup(value);
                break;
            }
        }
    }
    if (!location || !*location) {
        free(location);
        location = xstrdup("us-central1");
    }
    return location;
}

/* The API host of a location: the global endpoint fronts every region. */
static char *vertex_api_host(const char *location)
{
    if (strcmp(location, "global") == 0)
        return xstrdup("aiplatform.googleapis.com");
    return xasprintf("%s-aiplatform.googleapis.com", location);
}

static char *vertex_base_url(void)
{
    char *override = vertex_setting("base_url");
    if (override) {
        char *trimmed = url_trim_trailing_slashes(override);
        free(override);
        return trimmed;
    }

    char *project = vertex_project_id();
    if (!project || !*project) {
        free(project);
        return NULL;
    }

    char *location = vertex_location();
    char *host = vertex_api_host(location);
    char *url = xasprintf("https://%s/v1beta1/projects/%s/locations/%s/endpoints/openapi", host,
                          project, location);
    free(host);
    free(project);
    free(location);
    return url;
}

static void vertex_load_adc(struct vertex_auth_state *state)
{
    const char *path = getenv("GOOGLE_APPLICATION_CREDENTIALS");
    char *owned_path = NULL;
    if (!path || !*path) {
        const char *home = getenv("HOME");
        if (home && *home)
            owned_path = xasprintf("%s/.config/gcloud/application_default_credentials.json", home);
        path = owned_path;
    }
    json_t *root = path ? json_load_file(path, 0, NULL) : NULL;
    free(owned_path);
    if (!root)
        return;

    const char *type = json_string_value(json_object_get(root, "type"));
    if (type)
        state->adc_type = xstrdup(type);

    if (type && strcmp(type, "authorized_user") == 0) {
        const char *client_id = json_string_value(json_object_get(root, "client_id"));
        const char *client_secret = json_string_value(json_object_get(root, "client_secret"));
        const char *refresh_token = json_string_value(json_object_get(root, "refresh_token"));
        if (client_id && client_secret && refresh_token) {
            state->client_id = xstrdup(client_id);
            state->client_secret = xstrdup(client_secret);
            state->refresh_token = xstrdup(refresh_token);
        }
    }
    json_decref(root);
}

static int vertex_refresh_authorized_user(struct vertex_auth_state *state)
{
    if (!state->client_id || !state->client_secret || !state->refresh_token)
        return -1;

    char *fields = xasprintf("client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
                             state->client_id, state->client_secret, state->refresh_token);

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(fields);
        return -1;
    }
    struct vertex_buf buf = {0};
    curl_easy_setopt(curl, CURLOPT_URL, "https://oauth2.googleapis.com/token");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, vertex_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    free(fields);

    int ok = 0;
    if (result == CURLE_OK && status == 200 && buf.data) {
        json_t *root = json_loads(buf.data, 0, NULL);
        if (root) {
            const char *access_token = json_string_value(json_object_get(root, "access_token"));
            json_int_t expires_in = json_integer_value(json_object_get(root, "expires_in"));
            if (access_token && *access_token) {
                free(state->token);
                state->token = xstrdup(access_token);
                if (expires_in <= 0)
                    expires_in = 3600;
                state->expiry = time(NULL) + (time_t)expires_in - 60;
                ok = 1;
            }
            json_decref(root);
        }
    }
    free(buf.data);
    return ok ? 0 : -1;
}

static int vertex_refresh_gcloud(struct vertex_auth_state *state)
{
    FILE *fp = popen("gcloud auth print-access-token 2>/dev/null", "r");
    if (!fp)
        return -1;
    char buffer[512];
    size_t n = fread(buffer, 1, sizeof(buffer) - 1, fp);
    pclose(fp);
    if (n == 0)
        return -1;
    buffer[n] = '\0';
    /* Trim trailing whitespace. */
    while (n > 0 && (buffer[n - 1] == '\n' || buffer[n - 1] == '\r' || buffer[n - 1] == ' '))
        buffer[--n] = '\0';
    if (n == 0)
        return -1;
    free(state->token);
    state->token = xstrdup(buffer);
    state->expiry = time(NULL) + 3000;
    return 0;
}

static int vertex_auth_refresh(struct vertex_auth_state *state)
{
    if (state->static_token)
        return state->token ? 0 : -1;
    if (state->token && time(NULL) < state->expiry)
        return 0;
    if (state->client_id && state->refresh_token) {
        if (vertex_refresh_authorized_user(state) == 0)
            return 0;
    }
    if (vertex_refresh_gcloud(state) == 0)
        return 0;
    return -1;
}

static int vertex_auth_prepare(void *auth, int allow_refresh, http_tick_cb tick, void *tick_user)
{
    (void)allow_refresh;
    (void)tick;
    (void)tick_user;
    struct vertex_auth_state *state = auth;
    if (state->static_token)
        return state->token ? 0 : -1;
    if (state->token && time(NULL) < state->expiry)
        return 0;
    return vertex_auth_refresh(state) == 0 ? 0 : -1;
}

static char **vertex_auth_headers(const void *auth, const char *session_id, int streaming)
{
    (void)session_id;
    (void)streaming;
    struct vertex_auth_state *state = (struct vertex_auth_state *)auth;
    if (!state->token)
        vertex_auth_refresh(state);
    char *authorization = state->token ? xasprintf("Authorization: Bearer %s", state->token)
                                       : xstrdup("Authorization: Bearer");
    char **headers = xcalloc(2, sizeof(*headers));
    headers[0] = authorization;
    headers[1] = NULL;
    return headers;
}

static int vertex_auth_recover(void *auth, int *retried, http_tick_cb tick, void *tick_user)
{
    (void)tick;
    (void)tick_user;
    struct vertex_auth_state *state = auth;
    if (*retried || state->static_token)
        return 0;
    *retried = 1;
    if (vertex_auth_refresh(state) == 0)
        return 1;
    return 0;
}

static char *vertex_auth_unauthorized_message(void *auth)
{
    (void)auth;
    return xstrdup("Vertex AI authentication failed (set HAX_VERTEX_API_KEY or configure gcloud ADC)");
}

static void vertex_auth_destroy(void *auth)
{
    struct vertex_auth_state *state = auth;
    free(state->token);
    free(state->client_id);
    free(state->client_secret);
    free(state->refresh_token);
    free(state->adc_type);
    free(state);
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
    struct vertex_auth_state *state = xcalloc(1, sizeof(*state));

    char *prefix = xasprintf("providers.%s", def->id);
    const char *api_key_env = def->pinned ? def->api_key_env : config_scoped_str(prefix, "api_key_env");
    if (!def->pinned && (!api_key_env || !*api_key_env))
        api_key_env = def->api_key_env;
    const char *api_key = provider_api_key(prefix, api_key_env);
    if (api_key && *api_key) {
        state->static_token = 1;
        state->token = xstrdup(api_key);
    } else {
        vertex_load_adc(state);
    }
    free(prefix);

    out->state = state;
    out->ops = &VERTEX_AUTH_OPS;
    return 0;
}

char *vertex_rewrite_model(const char *model)
{
    if (!model || !*model || strchr(model, '/'))
        return NULL;
    return xasprintf("google/%s", model);
}

struct provider *vertex_provider_new(const struct provider_def *def)
{
    char *base_url = vertex_base_url();
    if (!base_url) {
        hax_err("provider '%s': VERTEXAI_PROJECT not set (set HAX_VERTEX_PROJECT or VERTEXAI_PROJECT)", def->id);
        return NULL;
    }

    struct provider_def copy = *def;
    copy.base_url = base_url;
    copy.pinned = 1;

    struct provider *provider = http_provider_new(&copy);
    free(base_url);
    if (!provider)
        return NULL;

    provider->list_models = NULL;
    return provider;
}

void vertex_prepare_availability(const struct provider_def *def, struct provider_availability *out)
{
    memset(out, 0, sizeof(*out));
    char *base_url = vertex_base_url();
    if (!base_url) {
        out->available = 0;
        out->reason = xstrdup("VERTEXAI_PROJECT not set");
        return;
    }

    const char *token_env = getenv("HAX_VERTEX_API_KEY");
    if (token_env && *token_env) {
        free(base_url);
        out->available = 1;
        return;
    }
    /* Check ADC or gcloud. */
    const char *home = getenv("HOME");
    char *adc_path = home ? xasprintf("%s/.config/gcloud/application_default_credentials.json", home) : NULL;
    struct stat st;
    int has_adc = adc_path && stat(adc_path, &st) == 0;
    free(adc_path);

    /* Also allow GOOGLE_APPLICATION_CREDENTIALS. */
    const char *gac = getenv("GOOGLE_APPLICATION_CREDENTIALS");
    if (gac && *gac) {
        if (stat(gac, &st) == 0)
            has_adc = 1;
    }

    int has_gcloud = (system("gcloud auth print-access-token >/dev/null 2>&1") == 0);

    free(base_url);
    if (has_adc || has_gcloud) {
        out->available = 1;
    } else {
        out->available = 0;
        out->reason = xstrdup("no Vertex AI credentials (set HAX_VERTEX_API_KEY or configure gcloud ADC)");
    }
}
