/* SPDX-License-Identifier: MIT */
#include "providers/vertex.h"

#include <ctype.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/typecheck-gcc.h>
#include <sys/stat.h>

#include "busy.h"
#include "config.h"
#include "diag.h"
#include "provider.h"
#include "session.h"
#include "xalloc.h"
#include "providers/http_provider.h"
#include "providers/provider_config.h"
#include "providers/registry.h"
#include "providers/usage_render.h"
#include "render/progress.h"
#include "system/spawn.h"
#include "terminal/ansi.h"
#include "terminal/ui.h"
#include "text/url.h"
#include "transport/http.h"

struct vertex_auth_state {
    int static_token;
    char *token;
    time_t expiry;

    char *client_id;
    char *client_secret;
    char *refresh_token;

    char *adc_type;
    char *gcloud_host; /* owned; the host holding the gcloud credentials, NULL for this machine */
};

struct vertex_buf {
    char *data;
    size_t len;
    size_t cap;
};

/* A gcloud token is a bearer string of a few hundred bytes; the probe answers the picker, which
 * waits on the foreground thread, so it gets the shorter budget. */
#define VERTEX_TOKEN_MAX_BYTES         8192
#define VERTEX_GCLOUD_TIMEOUT_MS       30000
#define VERTEX_GCLOUD_PROBE_TIMEOUT_MS 10000

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

    char *fields =
        xasprintf("client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
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

/* Where gcloud runs: NULL for this machine, otherwise the host that holds the credentials. */
static const char *vertex_gcloud_host(void)
{
    const char *host = config_scoped_str("providers.vertex-ai", "gcloud_host");
    return host && *host ? host : NULL;
}

/* Ask gcloud for an access token, here or on `host` over SSH. Returns the token text, or NULL
 * when the command failed. The caller frees. */
static char *vertex_gcloud_token(const char *host, int timeout_ms)
{
    /* No terminal and no X11 forwarding: the command prints one line, and a forwarding channel
     * the remote cannot set up only risks stalling the client that waits for it. */
    const char *const remote[] = {
        "ssh",
        "-T",
        "-x",
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=5",
        host,
        "gcloud",
        "auth",
        "print-access-token",
        NULL,
    };

    /* The gcloud command is the tail of the ssh command, so a local run is that same tail. */
    const char *const *cmd = remote;
    if (!host) {
        while (*++cmd)
            ;
        ++cmd;
    }

    size_t len = 0;
    char *output = spawn_capture_stdout(cmd, VERTEX_TOKEN_MAX_BYTES, timeout_ms, &len);
    if (!output)
        return NULL;
    while (len > 0 && isspace((unsigned char)output[len - 1]))
        output[--len] = '\0';
    if (len == 0) {
        free(output);
        return NULL;
    }
    return output;
}

static int vertex_refresh_gcloud(struct vertex_auth_state *state)
{
    char *token = vertex_gcloud_token(state->gcloud_host, VERTEX_GCLOUD_TIMEOUT_MS);
    if (!token)
        return -1;
    free(state->token);
    state->token = token;
    state->expiry = time(NULL) + 3000;
    return 0;
}

static int vertex_auth_refresh(struct vertex_auth_state *state)
{
    if (state->static_token)
        return state->token ? 0 : -1;
    if (state->token && time(NULL) < state->expiry)
        return 0;
    /* A configured host owns the credentials, so the local ones do not apply. */
    if (state->gcloud_host)
        return vertex_refresh_gcloud(state) == 0 ? 0 : -1;
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
    struct vertex_auth_state *state = auth;
    if (state->gcloud_host) {
        if (state->token)
            return xasprintf("Vertex AI authentication failed (the token from gcloud on '%s' was "
                             "rejected)",
                             state->gcloud_host);
        return xasprintf("Vertex AI authentication failed (no token from gcloud on '%s', check "
                         "SSH access)",
                         state->gcloud_host);
    }
    return xstrdup(
        "Vertex AI authentication failed (set HAX_VERTEX_API_KEY or configure gcloud ADC)");
}

static void vertex_auth_destroy(void *auth)
{
    struct vertex_auth_state *state = auth;
    free(state->token);
    free(state->client_id);
    free(state->client_secret);
    free(state->refresh_token);
    free(state->adc_type);
    free(state->gcloud_host);
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
    const char *api_key_env =
        def->pinned ? def->api_key_env : config_scoped_str(prefix, "api_key_env");
    if (!def->pinned && (!api_key_env || !*api_key_env))
        api_key_env = def->api_key_env;
    const char *api_key = provider_api_key(prefix, api_key_env);
    if (api_key && *api_key) {
        state->static_token = 1;
        state->token = xstrdup(api_key);
    } else {
        const char *host = vertex_gcloud_host();
        if (host) {
            /* The credentials live on the other machine, so the local ones do not apply. */
            state->gcloud_host = xstrdup(host);
        } else {
            vertex_load_adc(state);
        }
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
        hax_err(
            "provider '%s': VERTEXAI_PROJECT not set (set HAX_VERTEX_PROJECT or VERTEXAI_PROJECT)",
            def->id);
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
    (void)def;
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

    const char *host = vertex_gcloud_host();
    if (host) {
        /* The credentials live on the other machine; the local files say nothing about them. */
        int has_gcloud = vertex_gcloud_token(host, VERTEX_GCLOUD_PROBE_TIMEOUT_MS) != NULL;
        free(base_url);
        out->available = has_gcloud;
        if (!has_gcloud)
            out->reason = xasprintf("no token from gcloud on '%s' (check SSH access)", host);
        return;
    }

    /* Check ADC or gcloud. */
    const char *home = getenv("HOME");
    char *adc_path =
        home ? xasprintf("%s/.config/gcloud/application_default_credentials.json", home) : NULL;
    struct stat st;
    int has_adc = adc_path && stat(adc_path, &st) == 0;
    free(adc_path);

    /* Also allow GOOGLE_APPLICATION_CREDENTIALS. */
    const char *gac = getenv("GOOGLE_APPLICATION_CREDENTIALS");
    if (gac && *gac) {
        if (stat(gac, &st) == 0)
            has_adc = 1;
    }

    int has_gcloud = vertex_gcloud_token(NULL, VERTEX_GCLOUD_PROBE_TIMEOUT_MS) != NULL;

    free(base_url);
    if (has_adc || has_gcloud) {
        out->available = 1;
    } else {
        out->available = 0;
        out->reason =
            xstrdup("no Vertex AI credentials (set HAX_VERTEX_API_KEY or configure gcloud ADC)");
    }
}

/* ---------- /usage (Cloud Monitoring) ---------- */

#define VERTEX_USAGE_TIMEOUT_S 30
#define VERTEX_USAGE_ROWS_MAX  32

struct vertex_quota_row {
    char *metric;
    long usage;
    long limit;
    time_t reset_at;
    int has_limit;
    int has_usage;
};

static char *vertex_monitoring_base_url(void)
{
    const char *envs[] = {"HAX_VERTEX_MONITORING_URL", "HAX_VERTEX_MONITORING_BASE_URL",
                          "HAX_VERTEX_USAGE_URL", "HAX_VERTEX_MONITORING_ENDPOINT", NULL};
    const char *keys[] = {"monitoring_url", "monitoring_base_url", "usage_url",
                          "monitoring_endpoint", NULL};
    for (size_t i = 0; envs[i]; i++) {
        const char *value = getenv(envs[i]);
        if (value && *value)
            return xstrdup(value);
    }
    if (keys[0]) {
        for (size_t i = 0; keys[i]; i++) {
            char *key = xasprintf("providers.vertex-ai.%s", keys[i]);
            const char *configured = config_scoped_str("providers.vertex-ai", keys[i]);
            free(key);
            if (configured && *configured)
                return xstrdup(configured);
        }
    }
    return NULL;
}

static time_t vertex_parse_rfc3339(const char *text)
{
    if (!text || !*text)
        return (time_t)-1;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    /* Accept "2026-08-21T21:14:51Z" or with fractional seconds. */
    if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
               &tm.tm_min, &tm.tm_sec) != 6)
        return (time_t)-1;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    /* Skip fractional seconds if present. */
    const char *p = text + 19;
    if (*p == '.') {
        if (p[1] < '0' || p[1] > '9')
            return (time_t)-1;
        for (p++; *p >= '0' && *p <= '9'; p++)
            ;
    }
    if (*p != 'Z')
        return (time_t)-1;

    int year = tm.tm_year + 1900;
    int month = tm.tm_mon + 1;
    int day = tm.tm_mday;
    int hour = tm.tm_hour;
    int minute = tm.tm_min;
    int second = tm.tm_sec;

    int shifted_year = year - (month <= 2);
    int era = shifted_year / 400;
    int year_of_era = shifted_year - era * 400;
    int day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    long long days = (long long)era * 146097 + day_of_era - 719468;
    return (time_t)(days * 86400 + hour * 3600 + minute * 60 + second);
}

static long vertex_json_int_value(const json_t *value)
{
    if (!value)
        return 0;
    if (json_is_integer(value))
        return (long)json_integer_value(value);
    if (json_is_string(value)) {
        char *end = NULL;
        long result = strtol(json_string_value(value), &end, 10);
        if (end && *end == '\0')
            return result;
    }
    if (json_is_real(value))
        return (long)json_real_value(value);
    /* Jansson object with int64Value/doubleValue. */
    if (json_is_object(value)) {
        json_t *iv = json_object_get(value, "int64Value");
        if (iv)
            return vertex_json_int_value(iv);
        json_t *dv = json_object_get(value, "doubleValue");
        if (dv)
            return vertex_json_int_value(dv);
    }
    return 0;
}

static const char *vertex_time_series_metric(const json_t *series)
{
    json_t *metric = json_object_get(series, "metric");
    if (!json_is_object(metric))
        return NULL;
    json_t *labels = json_object_get(metric, "labels");
    if (json_is_object(labels)) {
        const char *value = json_string_value(json_object_get(labels, "quota_metric"));
        if (value && *value)
            return value;
    }
    return json_string_value(json_object_get(metric, "type"));
}

static time_t vertex_time_series_end(const json_t *series)
{
    json_t *points = json_object_get(series, "points");
    if (!json_is_array(points) || json_array_size(points) == 0)
        return time(NULL) + 60;
    json_t *point = json_array_get(points, 0);
    json_t *interval = json_object_get(point, "interval");
    const char *end = interval ? json_string_value(json_object_get(interval, "endTime")) : NULL;
    time_t parsed = vertex_parse_rfc3339(end);
    if (parsed < 0)
        return time(NULL) + 60;
    return parsed;
}

static long vertex_time_series_value(const json_t *series)
{
    json_t *points = json_object_get(series, "points");
    if (!json_is_array(points) || json_array_size(points) == 0)
        return 0;
    json_t *point = json_array_get(points, 0);
    return vertex_json_int_value(json_object_get(point, "value"));
}

static void vertex_collect_rows(json_t *root, int is_limit, struct vertex_quota_row *rows,
                                size_t *n_rows)
{
    if (!json_is_object(root))
        return;
    json_t *series_list = json_object_get(root, "timeSeries");
    if (!json_is_array(series_list))
        return;
    size_t i;
    for (i = 0; i < json_array_size(series_list); i++) {
        json_t *series = json_array_get(series_list, i);
        if (!json_is_object(series))
            continue;
        const char *metric_name = vertex_time_series_metric(series);
        if (!metric_name || !*metric_name)
            continue;

        /* Find existing row. */
        size_t r;
        struct vertex_quota_row *row = NULL;
        for (r = 0; r < *n_rows; r++) {
            if (rows[r].metric && strcmp(rows[r].metric, metric_name) == 0) {
                row = &rows[r];
                break;
            }
        }
        if (!row) {
            if (*n_rows >= VERTEX_USAGE_ROWS_MAX)
                continue;
            row = &rows[*n_rows];
            memset(row, 0, sizeof(*row));
            row->metric = xstrdup(metric_name);
            row->reset_at = vertex_time_series_end(series);
            (*n_rows)++;
        }
        long value = vertex_time_series_value(series);
        if (is_limit) {
            row->limit = value;
            row->has_limit = 1;
        } else {
            row->usage = value;
            row->has_usage = 1;
            if (row->reset_at == 0)
                row->reset_at = vertex_time_series_end(series);
        }
    }
}

static char *vertex_build_url(const char *base_or_full, const char *project, int is_limit)
{
    if (base_or_full && strstr(base_or_full, "timeSeries"))
        return xstrdup(base_or_full);

    const char *metric = is_limit ? "serviceruntime.googleapis.com/quota/limit"
                                  : "serviceruntime.googleapis.com/quota/rate/net_usage";
    time_t now = time(NULL);
    char start[64], end[64];
    struct tm stm, etm;
    gmtime_r(&now, &etm);
    strftime(end, sizeof(end), "%Y-%m-%dT%H:%M:%SZ", &etm);
    time_t start_time = now - 300;
    gmtime_r(&start_time, &stm);
    strftime(start, sizeof(start), "%Y-%m-%dT%H:%M:%SZ", &stm);

    char *filter = NULL;
    if (is_limit)
        filter = xasprintf(
            "metric.type=\"%s\"%%20AND%%20resource.labels.service=\"aiplatform.googleapis.com\"",
            metric);
    else
        filter = xasprintf(
            "metric.type=\"%s\"%%20AND%%20resource.labels.service=\"aiplatform.googleapis.com\"",
            metric);

    if (base_or_full && *base_or_full) {
        /* Treat as base URL: append path if it doesn't already end with timeSeries. */
        char *trimmed = url_trim_trailing_slashes(base_or_full);
        char *url =
            xasprintf("%s/v3/projects/%s/"
                      "timeSeries?filter=%s&interval.startTime=%s&interval.endTime=%s&aggregation."
                      "alignmentPeriod=60s&aggregation.perSeriesAligner=ALIGN_SUM",
                      trimmed, project, filter, start, end);
        free(trimmed);
        free(filter);
        return url;
    }

    char *url = xasprintf("https://monitoring.googleapis.com/v3/projects/%s/"
                          "timeSeries?filter=%s&interval.startTime=%s&interval.endTime=%s&"
                          "aggregation.alignmentPeriod=60s&aggregation.perSeriesAligner=ALIGN_SUM",
                          project, filter, start, end);
    free(filter);
    return url;
}

int vertex_query_usage(struct provider *provider)
{
    const struct http_auth_source *auth = http_provider_auth(provider);
    struct busy *busy = busy_begin("fetching usage...");
    if (auth->ops->prepare(auth->state, 1, busy_tick, NULL) != 0) {
        if (!busy_end(busy)) {
            char *message = auth->ops->unauthorized_message(auth->state);
            ui_error("%s", message);
            free(message);
        }
        return -1;
    }

    char *project = vertex_project_id();
    if (!project || !*project) {
        busy_end(busy);
        ui_error("Vertex AI project not set (set HAX_VERTEX_PROJECT or VERTEXAI_PROJECT)");
        free(project);
        return -1;
    }

    char *base_or_full = vertex_monitoring_base_url();

    struct vertex_quota_row rows[VERTEX_USAGE_ROWS_MAX];
    memset(rows, 0, sizeof(rows));
    size_t n_rows = 0;

    /* Usage request. */
    char *usage_url = vertex_build_url(base_or_full, project, 0);
    char *limit_url = vertex_build_url(base_or_full, project, 1);

    char **headers = NULL;
    /* Reuse vertex auth token. */
    /* auth->state is vertex_auth_state; call headers? Use http_provider? We'll build manually. */
    /* Get token by refreshing through auth ops not exposing token. Instead call prepare and use
     * auth headers via vertex_auth_headers? Not accessible. Use http_provider_auth and cast? */
    /* We'll call auth->ops->headers to get headers. */
    char **auth_headers = auth->ops->headers(auth->state, NULL, 0);
    /* auth_headers includes Authorization. Build full header list. */
    const char *fixed[] = {"Accept: application/json", NULL};
    headers = string_array_concat((const char *const *)auth_headers, fixed);
    string_array_free(auth_headers);

    char *usage_body = NULL, *limit_body = NULL;
    long usage_status = 0, limit_status = 0;
    int request_result = http_get(usage_url, (const char *const *)headers, VERTEX_USAGE_TIMEOUT_S,
                                  0, busy_tick, NULL, &usage_body, &usage_status);
    if (request_result == 0 && usage_body)
        http_get(limit_url, (const char *const *)headers, VERTEX_USAGE_TIMEOUT_S, 0, busy_tick,
                 NULL, &limit_body, &limit_status);

    int cancelled = busy_end(busy);
    string_array_free(headers);

    if (cancelled) {
        free(usage_body);
        free(limit_body);
        free(usage_url);
        free(limit_url);
        free(base_or_full);
        free(project);
        for (size_t i = 0; i < n_rows; i++)
            free(rows[i].metric);
        return -1;
    }

    if (request_result != 0 || !usage_body) {
        if (usage_status == 401) {
            char *message = auth->ops->unauthorized_message(auth->state);
            ui_error("%s", message);
            free(message);
        } else {
            ui_error("failed to fetch usage from %s", usage_url);
        }
        free(usage_body);
        free(limit_body);
        free(usage_url);
        free(limit_url);
        free(base_or_full);
        free(project);
        for (size_t i = 0; i < n_rows; i++)
            free(rows[i].metric);
        return -1;
    }

    json_error_t error;
    json_t *usage_root = json_loads(usage_body, 0, &error);
    if (usage_root)
        vertex_collect_rows(usage_root, 0, rows, &n_rows);
    json_t *limit_root = limit_body ? json_loads(limit_body, 0, &error) : NULL;
    if (limit_root)
        vertex_collect_rows(limit_root, 1, rows, &n_rows);

    printf(ANSI_DIM "vertex-ai");
    /* Try to show project. */
    printf(" | %s", project);
    printf(ANSI_RESET "\n");

    int printed = 0;
    for (size_t i = 0; i < n_rows; i++) {
        if (!rows[i].has_usage && !rows[i].has_limit)
            continue;
        const char *raw = rows[i].metric;
        const char *display = raw;
        const char *slash = strrchr(raw, '/');
        if (slash && slash[1])
            display = slash + 1;
        char *label = xstrdup(display);
        struct usage_window window;
        window.label = label;
        if (rows[i].has_limit && rows[i].limit > 0)
            window.used_percent = (double)rows[i].usage * 100.0 / (double)rows[i].limit;
        else
            window.used_percent = 0;
        window.reset_at = rows[i].reset_at ? rows[i].reset_at : time(NULL) + 60;
        window.note = rows[i].has_limit ? NULL : "no limit reported";
        usage_window_print(&window);
        free(label);
        printed = 1;
    }
    if (!printed) {
        printf("  " ANSI_DIM "no usage data reported" ANSI_RESET "\n");
    }

    json_decref(usage_root);
    json_decref(limit_root);
    free(usage_body);
    free(limit_body);
    free(usage_url);
    free(limit_url);
    free(base_or_full);
    free(project);
    for (size_t i = 0; i < n_rows; i++)
        free(rows[i].metric);

    /* Local aggregate across this project's sessions. */
    struct session_local_totals local;
    session_aggregate_usage("vertex-ai", &local);
    /* The registered rows carry the grammar: quota_tokens takes the k/m suffixes the
     * /config picker advertises, and both rows resolve their environment variable through
     * the config registry. */
    long quota_tokens = config_tokens("providers.vertex-ai.quota_tokens");
    const char *quota_cost_str = config_str("providers.vertex-ai.quota_cost");
    double quota_cost = quota_cost_str ? strtod(quota_cost_str, NULL) : 0.0;

    printf(ANSI_DIM "local" ANSI_RESET "\n");
    long total_tokens = local.input_tokens + local.output_tokens;
    if (quota_tokens > 0) {
        double pct = (double)total_tokens * 100.0 / (double)quota_tokens;
        printf("  ");
        progress_bar_print(pct / 100.0, 20);
        printf(" " ANSI_DIM "%3d%% used | %ld / %ld tokens" ANSI_RESET "\n", (int)(pct + 0.5),
               total_tokens, quota_tokens);
    } else if (quota_cost > 0 && total_tokens > 0 && local.spend > 0) {
        double avg = local.spend / (double)total_tokens;
        long inferred = (long)(quota_cost / avg);
        double pct = inferred > 0 ? (double)total_tokens * 100.0 / (double)inferred : 0.0;
        printf("  ");
        progress_bar_print(pct / 100.0, 20);
        printf(" " ANSI_DIM "%3d%% used | %ld / ~%ld tokens (est.)" ANSI_RESET "\n",
               (int)(pct + 0.5), total_tokens, inferred);
    } else
        printf("  " ANSI_DIM "%-*s%ld tokens" ANSI_RESET "\n", USAGE_LABEL_WIDTH, "tokens",
               total_tokens);
    if (quota_cost > 0) {
        double pct = local.spend * 100.0 / quota_cost;
        printf("  ");
        progress_bar_print(pct / 100.0, 20);
        printf(" " ANSI_DIM "%3d%% used | $%.4f / $%.4f" ANSI_RESET "\n", (int)(pct + 0.5),
               local.spend, quota_cost);
    } else if (local.spend > 0)
        printf("  " ANSI_DIM "%-*s$%.4f" ANSI_RESET "\n", USAGE_LABEL_WIDTH, "spend", local.spend);

    return 0;
}
