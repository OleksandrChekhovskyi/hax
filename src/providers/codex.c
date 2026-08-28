/* SPDX-License-Identifier: MIT */
#include "providers/codex.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "busy.h"
#include "effort.h"
#include "provider.h"
#include "util.h"
#include "version.h"
#include "providers/codex_auth.h"
#include "providers/http_provider.h"
#include "providers/usage_render.h"
#include "render/ctrl_strip.h"
#include "terminal/ansi.h"
#include "terminal/ui.h"
#include "transport/http.h"

/* Usage lives outside the codex API root the def's base_url names. */
#define CODEX_USAGE_ENDPOINT "https://chatgpt.com/backend-api/wham/usage"

#define CODEX_MODEL_TIMEOUT_SECONDS 5
#define CODEX_USAGE_TIMEOUT_SECONDS 30

/* The metadata probe runs for at most its own request timeout, so its token merely has to
 * outlive that plus slack — anything longer defers the probe needlessly. */
#define CODEX_PROBE_TOKEN_MARGIN_S 60

/* The models endpoint hides entries requiring a newer client version. A high synthetic version
 * exposes metadata for models that the responses endpoint already accepts. */
#define CODEX_MODEL_CLIENT_VERSION "999.0.0"

/* Both identities are required for models that the backend routes only to the official CLI. */
#define CODEX_ORIGINATOR "originator: codex_cli_rs"
#define CODEX_USER_AGENT "User-Agent: codex_cli_rs/0.144.1 hax/" HAX_VERSION

char **codex_static_headers(void)
{
    const char *fixed[] = {CODEX_ORIGINATOR, CODEX_USER_AGENT, NULL};
    return string_array_concat(fixed, NULL);
}

void codex_provider_reload_auth(struct provider *provider)
{
    codex_auth_session_reload(http_provider_auth(provider)->state);
}

/* Authenticated metadata GET with the source's credential lifecycle: headers rebuilt per
 * attempt and one bounded recovery after a 401. The caller runs prepare first. */
static int authorized_get(struct provider *provider, const char *url, int timeout_s,
                          http_tick_cb tick, void *tick_user, char **body, long *status)
{
    const struct http_auth_source *auth = http_provider_auth(provider);
    int result = -1;
    int auth_retried = 0;
    *body = NULL;
    do {
        free(*body);
        *body = NULL;
        char **headers = http_provider_metadata_headers(provider);
        result = http_get(url, (const char *const *)headers, timeout_s, 0, tick, tick_user, body,
                          status);
        string_array_free(headers);
    } while (*status == 401 && auth->ops->recover(auth->state, &auth_retried, tick, tick_user));
    return result;
}

static char *build_models_url(const struct provider *provider)
{
    return xasprintf("%s/models?client_version=%s", http_provider_base_url(provider),
                     CODEX_MODEL_CLIENT_VERSION);
}

static void parse_model_probe_response(const char *body, const char *model,
                                       struct model_info *model_info)
{
    json_t *root = json_loads(body, 0, NULL);
    if (!root)
        return;

    json_t *models = json_object_get(root, "models");
    if (!json_is_array(models)) {
        json_decref(root);
        return;
    }

    size_t i;
    json_t *entry;
    json_array_foreach(models, i, entry)
    {
        const char *slug = json_string_value(json_object_get(entry, "slug"));
        if (slug && strcmp(slug, model) == 0) {
            codex_parse_model(entry, model_info);
            break;
        }
    }
    json_decref(root);
}

int codex_probe_model(struct provider *provider, const char *model, struct model_probe *probe)
{
    if (!model || !*model)
        return -1;

    const struct http_auth_source *auth = http_provider_auth(provider);
    if (auth->ops->prepare(auth->state, 0, NULL, NULL) != 0)
        return -1;
    /* The probe only has to outlive its own short request, so its margin is much tighter than
     * the proactive-refresh window; a token the next request will rotate can still serve it. */
    if (codex_auth_session_expiring(auth->state, CODEX_PROBE_TOKEN_MARGIN_S)) {
        http_provider_defer_probe(provider);
        return -1;
    }
    probe->url = build_models_url(provider);
    probe->headers = http_provider_metadata_headers(provider);
    probe->timeout_s = CODEX_MODEL_TIMEOUT_SECONDS;
    probe->parse = parse_model_probe_response;
    return 0;
}

static void format_window_label(char *output, size_t output_size, long window_seconds)
{
    if (window_seconds <= 0)
        snprintf(output, output_size, "?");
    else if (window_seconds == 604800)
        snprintf(output, output_size, "weekly");
    else if (window_seconds == 86400)
        snprintf(output, output_size, "daily");
    else if (window_seconds < 60)
        snprintf(output, output_size, "%lds", window_seconds);
    else if (window_seconds < 3600)
        snprintf(output, output_size, "%ldm", window_seconds / 60);
    else if (window_seconds < 86400)
        snprintf(output, output_size, "%ldh", window_seconds / 3600);
    else
        snprintf(output, output_size, "%ldd", window_seconds / 86400);
}

static void print_usage_window(const char *fallback_label, json_t *window)
{
    if (!window || json_is_null(window))
        return;

    json_t *used_percent = json_object_get(window, "used_percent");
    json_t *reset_timestamp = json_object_get(window, "reset_at");
    json_t *duration = json_object_get(window, "limit_window_seconds");
    if (!json_is_number(used_percent) || !json_is_number(reset_timestamp)) {
        printf("  " ANSI_DIM "%-*s (unrecognized window shape)" ANSI_RESET "\n", USAGE_LABEL_WIDTH,
               fallback_label);
        return;
    }

    char label[32];
    if (json_is_integer(duration))
        format_window_label(label, sizeof(label), (long)json_integer_value(duration));
    else
        snprintf(label, sizeof(label), "%s", fallback_label);

    struct usage_window row = {
        .label = label,
        .used_percent = json_number_value(used_percent),
        .reset_at = (time_t)json_number_value(reset_timestamp),
    };
    usage_window_print(&row);
}

int codex_query_usage(struct provider *provider)
{
    const struct http_auth_source *auth = http_provider_auth(provider);
    /* The busy scope opens before prepare so a near-expiry token refresh shows the spinner and
     * honors Esc instead of freezing the command. */
    struct busy *busy = busy_begin("fetching usage...");
    if (auth->ops->prepare(auth->state, 1, busy_tick, NULL) != 0) {
        if (!busy_end(busy)) {
            char *message = auth->ops->unauthorized_message(auth->state);
            ui_error("%s", message);
            free(message);
        }
        return -1;
    }
    char *body = NULL;
    long status = 0;
    int result = authorized_get(provider, CODEX_USAGE_ENDPOINT, CODEX_USAGE_TIMEOUT_SECONDS,
                                busy_tick, NULL, &body, &status);
    int cancelled = busy_end(busy);
    if (cancelled) {
        free(body);
        return -1;
    }
    if (result != 0 || !body) {
        if (status == 401) {
            char *message = auth->ops->unauthorized_message(auth->state);
            ui_error("%s", message);
            free(message);
        } else {
            ui_error("failed to fetch usage from %s", CODEX_USAGE_ENDPOINT);
        }
        free(body);
        return -1;
    }

    json_error_t error;
    json_t *root = json_loads(body, 0, &error);
    free(body);
    if (!root) {
        ui_error("usage response is not valid JSON: %s", error.text);
        return -1;
    }

    const char *plan = json_string_value(json_object_get(root, "plan_type"));
    json_t *rate_limit = json_object_get(root, "rate_limit");

    printf(ANSI_DIM "codex");
    /* Email and plan arrive from the server (token claims and usage response); keep terminal
     * controls out of them. */
    const char *account_email = codex_auth_session_email(auth->state);
    if (account_email) {
        char *email = ctrl_strip_line_dup(account_email);
        printf(" · %s", email);
        free(email);
    }
    if (plan && *plan) {
        char *safe_plan = ctrl_strip_line_dup(plan);
        printf(" · %s", safe_plan);
        free(safe_plan);
    }
    printf(ANSI_RESET "\n");

    if (rate_limit && !json_is_null(rate_limit)) {
        print_usage_window("primary", json_object_get(rate_limit, "primary_window"));
        print_usage_window("secondary", json_object_get(rate_limit, "secondary_window"));
    } else {
        printf("  " ANSI_DIM "no rate-limit windows reported for this plan" ANSI_RESET "\n");
    }

    json_decref(root);
    return 0;
}

char *codex_model_catalog_error(long http_status)
{
    if (http_status >= 200 && http_status < 300)
        return xstrdup("codex sent an empty or truncated model catalog response");
    if (http_status != 0)
        return xasprintf("codex model catalog fetch failed (HTTP %ld)", http_status);
    return xstrdup("could not reach chatgpt.com to list models — check your network");
}

int codex_model_is_hidden(const json_t *entry)
{
    const char *visibility = json_string_value(json_object_get(entry, "visibility"));
    return visibility && strcmp(visibility, "hide") == 0;
}

void codex_parse_model(const json_t *entry, struct model_info *model)
{
    /* context_window is the served default; max_context_window is the ceiling the backend
     * sanctions for client-side overrides (catalog.models), and the fallback default. */
    json_t *max_context_window = json_object_get(entry, "max_context_window");
    if (json_is_integer(max_context_window) && json_integer_value(max_context_window) > 0)
        model->max_context = (long)json_integer_value(max_context_window);
    json_t *context_window = json_object_get(entry, "context_window");
    if (!json_is_integer(context_window) || json_integer_value(context_window) <= 0)
        context_window = max_context_window;
    if (json_is_integer(context_window) && json_integer_value(context_window) > 0)
        model->context = (long)json_integer_value(context_window);

    json_t *modalities = json_object_get(entry, "input_modalities");
    if (json_is_array(modalities)) {
        model->image_input = PROVIDER_CAP_NO;
        for (size_t i = 0; i < json_array_size(modalities); i++) {
            const char *modality = json_string_value(json_array_get(modalities, i));
            if (modality && strcmp(modality, "image") == 0)
                model->image_input = PROVIDER_CAP_YES;
        }
    }

    const char *description = json_string_value(json_object_get(entry, "description"));
    if (description && *description)
        model->description = xstrdup(description);

    codex_parse_model_efforts(entry, &model->efforts);
}

/* The catalog describes the official UI ladder: it omits accepted value "none" and may include
 * policy label "ultra", which the wire rejects. An absent ladder is unknown; an empty one denies
 * every effort. */
void codex_parse_model_efforts(const json_t *entry, struct effort_set *efforts)
{
    json_t *levels = json_object_get(entry, "supported_reasoning_levels");
    if (!json_is_array(levels))
        return;

    efforts->known = 1;
    if (json_array_size(levels) == 0)
        return;

    effort_set_add(efforts, "none");
    for (size_t i = 0; i < json_array_size(levels); i++) {
        json_t *level = json_array_get(levels, i);
        /* Accept the bare-string variant used by older catalog responses. */
        const char *effort = json_is_string(level)
                                 ? json_string_value(level)
                                 : json_string_value(json_object_get(level, "effort"));
        if (effort && strcmp(effort, "ultra") != 0)
            effort_set_add(efforts, effort);
    }
}

int codex_list_models(struct provider *provider, struct model_info **models_out,
                      size_t *model_count, char **error, http_tick_cb tick, void *tick_user)
{
    const struct http_auth_source *auth = http_provider_auth(provider);
    *models_out = NULL;
    *model_count = 0;
    if (auth->ops->prepare(auth->state, 1, tick, tick_user) != 0) {
        *error = auth->ops->unauthorized_message(auth->state);
        return -1;
    }

    char *url = build_models_url(provider);
    char *body = NULL;
    long http_status = 0;
    int result = authorized_get(provider, url, CODEX_MODEL_TIMEOUT_SECONDS, tick, tick_user, &body,
                                &http_status);
    free(url);
    if (result != 0) {
        *error = http_status == 401 ? auth->ops->unauthorized_message(auth->state)
                                    : codex_model_catalog_error(http_status);
        free(body);
        return -1;
    }

    json_t *root = json_loads(body, 0, NULL);
    free(body);
    if (!root) {
        *error = xstrdup("codex model catalog response is not valid JSON");
        return -1;
    }

    json_t *models = json_object_get(root, "models");
    if (!json_is_array(models)) {
        json_decref(root);
        *error = xstrdup("codex model catalog response has no model list");
        return -1;
    }

    size_t entry_count = json_array_size(models);
    struct model_info *listed_models =
        entry_count ? xmalloc(entry_count * sizeof(*listed_models)) : NULL;
    size_t listed_count = 0;
    size_t slug_count = 0;
    for (size_t i = 0; i < entry_count; i++) {
        json_t *entry = json_array_get(models, i);
        const char *slug = json_string_value(json_object_get(entry, "slug"));
        if (!slug || !*slug)
            continue;

        slug_count++;
        if (codex_model_is_hidden(entry))
            continue;

        model_info_init(&listed_models[listed_count]);
        listed_models[listed_count].id = xstrdup(slug);
        codex_parse_model(entry, &listed_models[listed_count]);
        listed_count++;
    }
    json_decref(root);

    /* A catalog containing only hidden models is valid; one with entries but no slugs is not. */
    if (entry_count > 0 && slug_count == 0) {
        free(listed_models);
        *error = xstrdup("codex model catalog response contains no usable model slugs");
        return -1;
    }

    *models_out = listed_models;
    *model_count = listed_count;
    return 0;
}

void codex_prepare_availability(const struct provider_def *def, struct provider_availability *out)
{
    (void)def;
    struct codex_auth auth;
    enum codex_auth_status status = codex_auth_load(&auth, NULL);
    codex_auth_release(&auth);

    out->available = status == CODEX_AUTH_OK;
    const char *reason = codex_auth_status_reason(status);
    out->reason = reason ? xstrdup(reason) : NULL;
}
