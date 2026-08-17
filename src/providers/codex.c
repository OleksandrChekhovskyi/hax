/* SPDX-License-Identifier: MIT */
#include "providers/codex.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "busy.h"
#include "config.h"
#include "effort.h"
#include "model_meta.h"
#include "provider.h"
#include "util.h"
#include "version.h"
#include "providers/codex_auth.h"
#include "providers/codex_settings.h"
#include "providers/config_provider.h"
#include "providers/responses_events.h"
#include "providers/responses_messages.h"
#include "render/progress.h"
#include "terminal/ansi.h"
#include "terminal/ui.h"
#include "terminal/width.h"
#include "transport/api_error.h"
#include "transport/http.h"
#include "transport/retry.h"

#define CODEX_RESPONSES_ENDPOINT "https://chatgpt.com/backend-api/codex/responses"
#define CODEX_USAGE_ENDPOINT     "https://chatgpt.com/backend-api/wham/usage"
#define CODEX_MODELS_ENDPOINT    "https://chatgpt.com/backend-api/codex/models"

#define CODEX_MODEL_TIMEOUT_SECONDS 5
#define CODEX_USAGE_TIMEOUT_SECONDS 30

/* The models endpoint hides entries requiring a newer client version. A high synthetic version
 * exposes metadata for models that the responses endpoint already accepts. */
#define CODEX_MODEL_CLIENT_VERSION "999.0.0"

/* Both identities are required for models that the backend routes only to the official CLI. */
#define CODEX_ORIGINATOR "originator: codex_cli_rs"
#define CODEX_USER_AGENT "User-Agent: codex_cli_rs/0.144.1 hax/" HAX_VERSION

/* Only the codex CLI can refresh the token; hax re-reads auth.json on the next request. */
#define CODEX_TOKEN_EXPIRED "codex token expired — run `codex` once to refresh, then retry"

struct codex {
    struct provider base;
    struct codex_auth auth;
    /* Set when a request is rejected as unauthenticated, so the next one re-reads auth.json and
     * picks up a token the codex CLI refreshed meanwhile. */
    int auth_stale;
    char *name;
    /* Mirrored from ~/.codex/config.toml; NULL when it names none. */
    char *default_model;
    char *default_effort;
    char *session_id; /* stable prompt-cache and request-routing key */
    char **extra_headers;
    json_t *extra_body;
};

/* Clearing the mark only once a different token is adopted keeps callers that cannot re-mark from
 * consuming it: model probes discard their HTTP status, so a probe's 401 is invisible here. */
static void reload_auth_if_stale(struct codex *codex)
{
    if (!codex->auth_stale)
        return;

    struct codex_auth refreshed;
    if (codex_auth_load(&refreshed, NULL) != CODEX_AUTH_OK)
        return;

    if (codex_auth_equal(&codex->auth, &refreshed)) {
        codex_auth_release(&refreshed);
        return;
    }

    codex_auth_release(&codex->auth);
    codex->auth = refreshed;
    codex->auth_stale = 0;
}

static char *build_request_body(const struct context *context, const char *provider,
                                const char *model, const char *cache_key, const json_t *extra_body)
{
    json_t *body = responses_build_body(context, provider, model);
    json_object_set_new(body, "text", json_pack("{s:s}", "verbosity", "low"));
    if (cache_key)
        json_object_set_new(body, "prompt_cache_key", json_string(cache_key));
    provider_extra_body_apply(body, extra_body);

    char *body_json = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    return body_json;
}

static int handle_sse_payload(const char *event_name, const char *data, void *parser)
{
    (void)event_name;
    responses_events_feed(parser, data);
    return 0;
}

static int codex_stream(struct provider *provider, const struct context *context, const char *model,
                        stream_cb callback, void *callback_user, http_tick_cb tick, void *tick_user)
{
    struct codex *codex = (struct codex *)provider;
    reload_auth_if_stale(codex);

    char *body = build_request_body(context, provider_stable_id(&codex->base), model,
                                    codex->session_id, codex->extra_body);
    if (!body)
        return -1;
    size_t body_len = strlen(body);

    char *authorization = xasprintf("Authorization: Bearer %s", codex->auth.access_token);
    char *account = xasprintf("chatgpt-account-id: %s", codex->auth.account_id);
    char *session = xasprintf("session-id: %s", codex->session_id);
    char *request_id = xasprintf("x-client-request-id: %s", codex->session_id);
    const char *fixed[] = {
        authorization,
        account,
        session,
        request_id,
        CODEX_ORIGINATOR,
        CODEX_USER_AGENT,
        "OpenAI-Beta: responses=experimental",
        "Accept: text/event-stream",
        "Content-Type: application/json",
        NULL,
    };
    char **headers = string_array_concat(fixed, (const char *const *)codex->extra_headers);
    free(authorization);
    free(account);
    free(session);
    free(request_id);

    struct retry_policy retry_policy = retry_policy_default();
    struct http_response response = {0};
    struct responses_events events = {0};
    int result = -1;

    for (int attempt = 0; attempt < retry_policy.max_attempts; attempt++) {
        memset(&response, 0, sizeof(response));
        responses_events_init(&events, callback, callback_user);
        result = http_sse_post(CODEX_RESPONSES_ENDPOINT, (const char *const *)headers, body,
                               body_len, retry_policy.idle_timeout_s, handle_sse_payload, &events,
                               tick, tick_user, &response);

        if (response.cancelled ||
            !retry_should_attempt(result, response.status, response.error_body) ||
            attempt + 1 >= retry_policy.max_attempts)
            break;

        long delay_ms = response.retry_after_ms > 0 ? response.retry_after_ms
                                                    : retry_delay_ms(&retry_policy, attempt);
        struct stream_event retry_event = {
            .kind = EV_RETRY,
            .u.retry = {.attempt = attempt + 1,
                        .max_attempts = retry_policy.max_attempts,
                        .delay_ms = delay_ms,
                        .http_status = (int)response.status},
        };
        callback(&retry_event, callback_user);

        free(response.error_body);
        response.error_body = NULL;
        responses_events_free(&events);
        if (retry_sleep_with_tick(delay_ms, tick, tick_user)) {
            response.cancelled = 1;
            break;
        }
    }

    if (!response.cancelled) {
        if (response.status == 401) {
            codex->auth_stale = 1;
            struct stream_event error_event = {
                .kind = EV_ERROR,
                .u.error = {.message = CODEX_TOKEN_EXPIRED, .http_status = 401},
            };
            callback(&error_event, callback_user);
        } else if (result != 0 || response.status < 200 || response.status >= 300) {
            char *message = format_api_error(response.status, response.error_body);
            struct stream_event error_event = {
                .kind = EV_ERROR,
                .u.error = {.message = message, .http_status = (int)response.status},
            };
            callback(&error_event, callback_user);
            free(message);
        } else {
            responses_events_finalize(&events);
        }
    }

    free(response.error_body);
    responses_events_free(&events);
    string_array_free(headers);
    free(body);
    return result;
}

/* Owned NULL-terminated headers for the JSON (non-stream) endpoints. */
static char **build_model_headers(const struct codex *codex)
{
    char *authorization = xasprintf("Authorization: Bearer %s", codex->auth.access_token);
    char *account = xasprintf("chatgpt-account-id: %s", codex->auth.account_id);
    const char *fixed[] = {
        authorization, account, CODEX_ORIGINATOR, CODEX_USER_AGENT, "Accept: application/json",
        NULL,
    };
    char **headers = string_array_concat(fixed, (const char *const *)codex->extra_headers);
    free(authorization);
    free(account);
    return headers;
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

static int codex_probe_model(struct provider *provider, const char *model,
                             struct model_probe *probe)
{
    struct codex *codex = (struct codex *)provider;
    if (!model || !*model)
        return -1;

    reload_auth_if_stale(codex);
    probe->url =
        xasprintf("%s?client_version=%s", CODEX_MODELS_ENDPOINT, CODEX_MODEL_CLIENT_VERSION);
    probe->headers = build_model_headers(codex);
    probe->timeout_s = CODEX_MODEL_TIMEOUT_SECONDS;
    probe->parse = parse_model_probe_response;
    return 0;
}

static void format_reset_time(char *output, size_t output_size, time_t reset_at)
{
    time_t now = time(NULL);
    struct tm reset_tm, now_tm;
    if (!localtime_r(&reset_at, &reset_tm) || !localtime_r(&now, &now_tm)) {
        snprintf(output, output_size, "?");
        return;
    }

    int same_day = reset_tm.tm_year == now_tm.tm_year && reset_tm.tm_yday == now_tm.tm_yday;
    if (same_day)
        strftime(output, output_size, "%H:%M", &reset_tm);
    else
        /* Zero-padded %d is preferable to the non-portable %-d. */
        strftime(output, output_size, "%a %b %d, %H:%M", &reset_tm);
}

#define USAGE_LABEL_WIDTH 6
#define USAGE_BAR_WIDTH   20
#define USAGE_BAR_COLUMN  (2 + USAGE_LABEL_WIDTH + 1)

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

    double used = json_number_value(used_percent);
    if (used < 0)
        used = 0;
    if (used > 100)
        used = 100;

    char reset_time[64];
    format_reset_time(reset_time, sizeof(reset_time), (time_t)json_number_value(reset_timestamp));

    char percent_text[32];
    int percent_width =
        snprintf(percent_text, sizeof(percent_text), " %3d%% used", (int)(used + 0.5));
    char reset_text[96];
    int reset_width = snprintf(reset_text, sizeof(reset_text), " · resets %s", reset_time);
    int row_width = USAGE_BAR_COLUMN + USAGE_BAR_WIDTH + percent_width + reset_width;

    printf("  " ANSI_DIM "%-*s" ANSI_RESET " ", USAGE_LABEL_WIDTH, label);
    progress_bar_print(used / 100.0, USAGE_BAR_WIDTH);
    if (row_width <= display_width()) {
        printf(ANSI_DIM "%s%s" ANSI_RESET "\n", percent_text, reset_text);
    } else {
        printf(ANSI_DIM "%s" ANSI_RESET "\n", percent_text);
        printf("%*s" ANSI_DIM "resets %s" ANSI_RESET "\n", USAGE_BAR_COLUMN, "", reset_time);
    }
}

static int codex_query_usage(struct provider *provider)
{
    struct codex *codex = (struct codex *)provider;
    reload_auth_if_stale(codex);

    char **headers = build_model_headers(codex);

    struct busy *busy = busy_begin("fetching usage...");
    char *body = NULL;
    long status = 0;
    int result = http_get(CODEX_USAGE_ENDPOINT, (const char *const *)headers,
                          CODEX_USAGE_TIMEOUT_SECONDS, 0, busy_tick, NULL, &body, &status);
    int cancelled = busy_end(busy);
    string_array_free(headers);
    if (cancelled) {
        free(body);
        return -1;
    }
    if (result != 0 || !body) {
        if (status == 401) {
            codex->auth_stale = 1;
            ui_error(CODEX_TOKEN_EXPIRED);
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
    if (codex->auth.email)
        printf(" · %s", codex->auth.email);
    if (plan && *plan)
        printf(" · %s", plan);
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

/* The provider-wide superset is narrowed by per-model catalog metadata before use. "ultra" is an
 * official-client policy label, not a reasoning value accepted by the API. */
static const char *const CODEX_EFFORTS[] = {"none", "low", "medium", "high", "xhigh", "max"};

static size_t codex_list_efforts(struct provider *provider, const char *const **efforts)
{
    (void)provider;
    *efforts = CODEX_EFFORTS;
    return sizeof(CODEX_EFFORTS) / sizeof(CODEX_EFFORTS[0]);
}

char *codex_model_catalog_error(long http_status)
{
    if (http_status == 401)
        return xstrdup(CODEX_TOKEN_EXPIRED);
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
    /* context_window is the served limit; max_context_window is only a fallback ceiling. */
    json_t *context_window = json_object_get(entry, "context_window");
    if (!json_is_integer(context_window) || json_integer_value(context_window) <= 0)
        context_window = json_object_get(entry, "max_context_window");
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

static int codex_list_models(struct provider *provider, struct model_info **models_out,
                             size_t *model_count, char **error, http_tick_cb tick, void *tick_user)
{
    struct codex *codex = (struct codex *)provider;
    *models_out = NULL;
    *model_count = 0;
    reload_auth_if_stale(codex);

    char *url =
        xasprintf("%s?client_version=%s", CODEX_MODELS_ENDPOINT, CODEX_MODEL_CLIENT_VERSION);
    char **headers = build_model_headers(codex);
    char *body = NULL;
    long http_status = 0;
    int result = http_get(url, (const char *const *)headers, CODEX_MODEL_TIMEOUT_SECONDS, 0, tick,
                          tick_user, &body, &http_status);
    string_array_free(headers);
    free(url);
    if (result != 0) {
        if (http_status == 401)
            codex->auth_stale = 1;
        *error = codex_model_catalog_error(http_status);
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

static void codex_destroy(struct provider *provider)
{
    struct codex *codex = (struct codex *)provider;
    model_meta_release(provider);
    codex_auth_release(&codex->auth);
    free(codex->name);
    free(codex->default_model);
    free(codex->default_effort);
    free(codex->session_id);
    string_array_free(codex->extra_headers);
    json_decref(codex->extra_body);
    free(codex);
}

struct provider *codex_provider_new(const char *id)
{
    static const char *const EXTRA_FIELDS[] = {"display_name", "extra_body", "extra_headers", NULL};
    provider_warn_unused_fields(id, id, 0, EXTRA_FIELDS);
    struct codex_auth auth;
    char *detail = NULL;
    enum codex_auth_status status = codex_auth_load(&auth, &detail);
    switch (status) {
    case CODEX_AUTH_OK:
        break;
    case CODEX_AUTH_NO_FILE:
        hax_err("cannot read %s — is the codex CLI installed and logged in?", detail);
        free(detail);
        return NULL;
    case CODEX_AUTH_BAD_JSON:
        hax_err("~/.codex/auth.json is not valid JSON: %s", detail);
        free(detail);
        return NULL;
    case CODEX_AUTH_NO_TOKENS:
        hax_err("auth.json missing tokens.access_token or tokens.account_id");
        free(detail);
        return NULL;
    }
    free(detail);

    char *default_model = NULL;
    char *default_effort = NULL;
    codex_load_settings(&default_model, &default_effort);

    struct codex *codex = xcalloc(1, sizeof(*codex));
    codex->auth = auth;
    codex->default_model = default_model;
    codex->default_effort = default_effort;
    char session_id[37];
    gen_uuid_v4(session_id);
    codex->session_id = xstrdup(session_id);
    codex->extra_headers = provider_extra_headers("providers.codex");
    codex->extra_body = provider_extra_body("providers.codex");

    const char *display_name = config_scoped_str("providers.codex", "display_name");
    codex->name = xstrdup(display_name && *display_name ? display_name : "codex");
    codex->base.name = codex->name;
    codex->base.id = id;
    /* Subscription responses report no cost, so estimate against equivalent OpenAI API rates. */
    codex->base.catalog_id = "openai";
    codex->base.default_model = codex->default_model;
    codex->base.default_effort = codex->default_effort;
    codex->base.stream = codex_stream;
    codex->base.query_usage = codex_query_usage;
    codex->base.list_models = codex_list_models;
    codex->base.list_efforts = codex_list_efforts;
    codex->base.probe_model = codex_probe_model;
    codex->base.destroy = codex_destroy;

    const char *configured_model = config_str("model");
    model_meta_refresh(&codex->base, (configured_model && *configured_model)
                                         ? configured_model
                                         : codex->default_model);
    return &codex->base;
}

static void codex_prepare_availability(const char *id, struct provider_availability *availability)
{
    (void)id;
    struct codex_auth auth;
    enum codex_auth_status status = codex_auth_load(&auth, NULL);
    codex_auth_release(&auth);

    availability->available = status == CODEX_AUTH_OK;
    availability->reason = codex_auth_status_reason(status);
}

const struct provider_factory PROVIDER_CODEX = {
    .id = "codex",
    .new = codex_provider_new,
    .prepare_availability = codex_prepare_availability,
};
