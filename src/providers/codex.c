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
#include "tool_schema.h"
#include "util.h"
#include "providers/codex_events.h"
#include "providers/codex_messages.h"
#include "render/progress.h"
#include "system/path.h"
#include "terminal/ansi.h"
#include "terminal/ui.h"
#include "text/base64.h"
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
#define CODEX_USER_AGENT "User-Agent: codex_cli_rs/0.144.1 hax/0.1"

struct codex {
    struct provider base;
    char *access_token;
    char *account_id;
    char *account_email;
    char *default_model;
    char *default_effort;
    char *session_id; /* stable prompt-cache and request-routing key */
};

static const char *skip_inline_whitespace(const char *cursor, const char *end)
{
    while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
        cursor++;
    return cursor;
}

static int toml_key_matches(const char *cursor, const char *end, const char *key)
{
    size_t key_len = strlen(key);
    if ((size_t)(end - cursor) < key_len || memcmp(cursor, key, key_len) != 0)
        return 0;

    cursor += key_len;
    return cursor == end || *cursor == '=' || *cursor == ' ' || *cursor == '\t';
}

static char *parse_toml_string(const char *value, const char *end)
{
    if (value >= end || (*value != '"' && *value != '\''))
        return NULL;

    char quote = *value++;
    struct buf result;
    buf_init(&result);

    while (value < end) {
        char byte = *value++;
        if (byte == quote)
            return buf_steal(&result);

        if (quote == '"' && byte == '\\' && value < end) {
            byte = *value++;
            switch (byte) {
            case 'b':
                byte = '\b';
                break;
            case 't':
                byte = '\t';
                break;
            case 'n':
                byte = '\n';
                break;
            case 'f':
                byte = '\f';
                break;
            case 'r':
                byte = '\r';
                break;
            case '"':
            case '\\':
                break;
            default:
                /* Preserve unsupported escaped bytes instead of rejecting otherwise usable Codex
                 * settings. */
                break;
            }
        }
        buf_append(&result, &byte, 1);
    }

    buf_free(&result);
    return NULL;
}

static char *parse_top_level_toml_string(const char *contents, size_t contents_len, const char *key)
{
    const char *cursor = contents;
    const char *end = contents + contents_len;

    while (cursor < end) {
        const char *line_end = memchr(cursor, '\n', (size_t)(end - cursor));
        if (!line_end)
            line_end = end;

        const char *assignment = skip_inline_whitespace(cursor, line_end);
        if (assignment < line_end && *assignment == '[')
            return NULL;
        if (assignment < line_end && *assignment != '#' &&
            toml_key_matches(assignment, line_end, key)) {
            assignment = skip_inline_whitespace(assignment + strlen(key), line_end);
            if (assignment < line_end && *assignment == '=') {
                assignment = skip_inline_whitespace(assignment + 1, line_end);
                return parse_toml_string(assignment, line_end);
            }
        }

        cursor = line_end < end ? line_end + 1 : end;
    }

    return NULL;
}

static void load_codex_settings(char **model, char **effort)
{
    *model = NULL;
    *effort = NULL;

    char *path = expand_home("~/.codex/config.toml");
    size_t contents_len = 0;
    char *contents = slurp_file(path, &contents_len);
    free(path);
    if (!contents)
        return;

    *model = parse_top_level_toml_string(contents, contents_len, "model");
    *effort = parse_top_level_toml_string(contents, contents_len, "model_reasoning_effort");
    free(contents);
}

/* This is an informational label, not authentication: the JWT from Codex's protected auth file is
 * decoded but not verified. Some login flows put email only in the namespaced profile claim. */
static char *extract_jwt_email(const char *jwt)
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
    if (!root)
        return NULL;

    const char *email = json_string_value(json_object_get(root, "email"));
    if (!email || !*email) {
        json_t *profile = json_object_get(root, "https://api.openai.com/profile");
        email = json_string_value(json_object_get(profile, "email"));
    }

    char *result = email && *email ? xstrdup(email) : NULL;
    json_decref(root);
    return result;
}

static json_t *build_tools(const struct tool_def *tools, size_t n_tools)
{
    json_t *definitions = json_array();
    for (size_t i = 0; i < n_tools; i++) {
        json_t *parameters = tool_schema_build(&tools[i]);
        json_array_append_new(definitions,
                              json_pack("{s:s, s:s, s:s, s:o}", "type", "function", "name",
                                        tools[i].name, "description", tools[i].description,
                                        "parameters", parameters));
    }
    return definitions;
}

static char *build_request_body(const struct context *context, const char *provider,
                                const char *model, const char *cache_key)
{
    json_t *include = json_array();
    json_array_append_new(include, json_string("reasoning.encrypted_content"));

    json_t *body = json_pack(
        "{s:s, s:b, s:b, s:s, s:o, s:o, s:{s:s}, s:s, s:b, s:o}", "model", model, "store", 0,
        "stream", 1, "instructions", context->system_prompt ? context->system_prompt : "", "input",
        codex_build_input_items(context->items, context->n_items, provider, model,
                                context->image_input),
        "include", include, "text", "verbosity", "low", "tool_choice", "auto",
        "parallel_tool_calls", 1, "tools", build_tools(context->tools, context->n_tools));

    if (cache_key)
        json_object_set_new(body, "prompt_cache_key", json_string(cache_key));
    if (context->effort)
        json_object_set_new(body, "reasoning",
                            json_pack("{s:s, s:s}", "effort", context->effort, "summary", "auto"));

    char *body_json = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    return body_json;
}

static int handle_sse_payload(const char *event_name, const char *data, void *parser)
{
    (void)event_name;
    codex_events_feed(parser, data);
    return 0;
}

static int codex_stream(struct provider *provider, const struct context *context, const char *model,
                        stream_cb callback, void *callback_user, http_tick_cb tick, void *tick_user)
{
    struct codex *codex = (struct codex *)provider;

    char *body = build_request_body(context, codex->base.name, model, codex->session_id);
    if (!body)
        return -1;
    size_t body_len = strlen(body);

    char *authorization = xasprintf("Authorization: Bearer %s", codex->access_token);
    char *account = xasprintf("chatgpt-account-id: %s", codex->account_id);
    char *session = xasprintf("session-id: %s", codex->session_id);
    char *request_id = xasprintf("x-client-request-id: %s", codex->session_id);
    const char *headers[] = {
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

    struct retry_policy retry_policy = retry_policy_default();
    struct http_response response = {0};
    struct codex_events events = {0};
    int result = -1;

    for (int attempt = 0; attempt < retry_policy.max_attempts; attempt++) {
        memset(&response, 0, sizeof(response));
        codex_events_init(&events, callback, callback_user);
        result = http_sse_post(CODEX_RESPONSES_ENDPOINT, headers, body, body_len,
                               retry_policy.idle_timeout_s, handle_sse_payload, &events, tick,
                               tick_user, &response);

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
        codex_events_free(&events);
        if (retry_sleep_with_tick(delay_ms, tick, tick_user)) {
            response.cancelled = 1;
            break;
        }
    }

    if (!response.cancelled) {
        if (response.status == 401) {
            struct stream_event error_event = {
                .kind = EV_ERROR,
                .u.error = {.message = "codex token expired — run `codex` "
                                       "once to refresh, then retry",
                            .http_status = 401},
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
            codex_events_finalize(&events);
        }
    }

    free(response.error_body);
    codex_events_free(&events);
    free(authorization);
    free(account);
    free(session);
    free(request_id);
    free(body);
    return result;
}

static char **build_model_headers(const struct codex *codex)
{
    char **headers = xcalloc(6, sizeof(*headers));
    headers[0] = xasprintf("Authorization: Bearer %s", codex->access_token);
    headers[1] = xasprintf("chatgpt-account-id: %s", codex->account_id);
    headers[2] = xstrdup(CODEX_ORIGINATOR);
    headers[3] = xstrdup(CODEX_USER_AGENT);
    headers[4] = xstrdup("Accept: application/json");
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

    char *authorization = xasprintf("Authorization: Bearer %s", codex->access_token);
    char *account = xasprintf("chatgpt-account-id: %s", codex->account_id);
    const char *headers[] = {
        authorization, account, CODEX_ORIGINATOR, CODEX_USER_AGENT, "Accept: application/json",
        NULL,
    };

    struct busy *busy = busy_begin("fetching usage...");
    char *body = NULL;
    long status = 0;
    int result = http_get(CODEX_USAGE_ENDPOINT, headers, CODEX_USAGE_TIMEOUT_SECONDS, 0, busy_tick,
                          NULL, &body, &status);
    int cancelled = busy_end(busy);
    free(authorization);
    free(account);
    if (cancelled) {
        free(body);
        return -1;
    }
    if (result != 0 || !body) {
        if (status == 401)
            ui_error("codex token expired — run `codex` once to refresh, then retry");
        else
            ui_error("failed to fetch usage from %s", CODEX_USAGE_ENDPOINT);
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
    if (codex->account_email)
        printf(" · %s", codex->account_email);
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
        return xstrdup("codex token expired — run `codex` once to refresh, then retry");
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
    free(codex->access_token);
    free(codex->account_id);
    free(codex->account_email);
    free(codex->default_model);
    free(codex->default_effort);
    free(codex->session_id);
    free(codex);
}

static int get_auth_tokens(json_t *root, const char **access_token, const char **account_id,
                           const char **id_token)
{
    json_t *tokens = json_object_get(root, "tokens");
    *access_token = json_string_value(json_object_get(tokens, "access_token"));
    *account_id = json_string_value(json_object_get(tokens, "account_id"));
    if (id_token)
        *id_token = json_string_value(json_object_get(tokens, "id_token"));
    return *access_token && **access_token && *account_id && **account_id;
}

struct provider *codex_provider_new(const char *name)
{
    (void)name;
    char *path = expand_home("~/.codex/auth.json");
    char *contents = slurp_file(path, NULL);
    if (!contents) {
        hax_err("cannot read %s — is the codex CLI installed and logged in?", path);
        free(path);
        return NULL;
    }
    free(path);

    json_error_t error;
    json_t *root = json_loads(contents, 0, &error);
    free(contents);
    if (!root) {
        hax_err("~/.codex/auth.json is not valid JSON: %s", error.text);
        return NULL;
    }

    const char *access_token;
    const char *account_id;
    const char *id_token;
    if (!get_auth_tokens(root, &access_token, &account_id, &id_token)) {
        hax_err("auth.json missing tokens.access_token or tokens.account_id");
        json_decref(root);
        return NULL;
    }

    char *default_model = NULL;
    char *default_effort = NULL;
    load_codex_settings(&default_model, &default_effort);
    if (default_model && !*default_model) {
        free(default_model);
        default_model = NULL;
    }
    if (default_effort && !*default_effort) {
        free(default_effort);
        default_effort = NULL;
    }

    struct codex *codex = xcalloc(1, sizeof(*codex));
    codex->access_token = xstrdup(access_token);
    codex->account_id = xstrdup(account_id);
    codex->account_email = extract_jwt_email(id_token);
    codex->default_model = default_model ? default_model : xstrdup("gpt-5.3-codex");
    codex->default_effort = default_effort;
    char session_id[37];
    gen_uuid_v4(session_id);
    codex->session_id = xstrdup(session_id);

    codex->base.name = "codex";
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

    json_decref(root);
    const char *configured_model = config_str("model");
    model_meta_refresh(&codex->base, (configured_model && *configured_model)
                                         ? configured_model
                                         : codex->default_model);
    return &codex->base;
}

static int codex_auth_available(const char *name, const char **reason)
{
    (void)name;
    char *path = expand_home("~/.codex/auth.json");
    if (!path) {
        if (reason)
            *reason = "no home directory";
        return 0;
    }

    char *contents = slurp_file(path, NULL);
    free(path);
    if (!contents) {
        if (reason)
            *reason = "codex CLI not logged in";
        return 0;
    }

    json_t *root = json_loads(contents, 0, NULL);
    free(contents);
    if (!root) {
        if (reason)
            *reason = "auth.json not valid JSON";
        return 0;
    }

    const char *access_token;
    const char *account_id;
    int available = get_auth_tokens(root, &access_token, &account_id, NULL);
    json_decref(root);
    if (!available && reason)
        *reason = "codex CLI not logged in";
    return available;
}

static void codex_prepare_availability(const char *name, struct provider_availability *availability)
{
    const char *reason = NULL;
    availability->available = codex_auth_available(name, &reason);
    availability->reason = reason;
}

const struct provider_factory PROVIDER_CODEX = {
    .name = "codex",
    .new = codex_provider_new,
    .prepare_availability = codex_prepare_availability,
};
