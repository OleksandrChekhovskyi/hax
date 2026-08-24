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
#include "providers/codex_login.h"
#include "providers/codex_settings.h"
#include "providers/config_provider.h"
#include "providers/responses_body.h"
#include "providers/responses_events.h"
#include "providers/stream_retry.h"
#include "providers/usage_render.h"
#include "render/ctrl_strip.h"
#include "terminal/ansi.h"
#include "terminal/ui.h"
#include "transport/http.h"

#define CODEX_RESPONSES_ENDPOINT "https://chatgpt.com/backend-api/codex/responses"
#define CODEX_USAGE_ENDPOINT     "https://chatgpt.com/backend-api/wham/usage"
#define CODEX_MODELS_ENDPOINT    "https://chatgpt.com/backend-api/codex/models"

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

/* Only the codex CLI can refresh a borrowed token; hax re-reads auth.json on the next request. */
#define CODEX_TOKEN_EXPIRED_CLI    "codex CLI token expired — rerun `codex`, or use /login"
#define CODEX_TOKEN_EXPIRED_HAX    "codex login expired — run /login again"
#define CODEX_TOKEN_REFRESH_FAILED "could not refresh the codex login — retry, or run /login"
/* The live auth was cleared by /logout and no fallback credentials have appeared since. */
#define CODEX_NOT_LOGGED_IN "codex is not logged in — run /login"
#define CODEX_ACCOUNT_CHANGED                                                                      \
    "the codex login belongs to a different account — run /login or /provider to switch"

struct codex {
    struct provider base;
    struct codex_auth auth;
    /* Set when a request with borrowed credentials is rejected as unauthenticated, so the next one
     * re-reads the codex CLI's auth.json and picks up a token it refreshed meanwhile. hax-owned
     * credentials refresh through codex_login_ensure_fresh instead. */
    int auth_stale;
    /* The last forced recovery failed transiently, so its 401 advises a retry, not /login. */
    int refresh_transient;
    /* Account this provider was constructed for (or explicitly switched to); credentials for any
     * other account are never adopted implicitly, even after the live auth is cleared. */
    char *account_pin;
    char *catalog_id; /* owned; configuration storage may be replaced at runtime */
    /* Reloading found credentials for a different account, so requests report that instead of
     * "not logged in". */
    int account_blocked;
    /* The metadata probe was skipped on an expiring managed token — the probe path cannot
     * refresh — and is re-launched after the next request's refresh opportunity. */
    int probe_deferred;
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

/* Called before each request so its Authorization header is built from a usable token. With
 * `allow_refresh` cleared only file-based reloads run — for callers that must not block on
 * network I/O, whose requests then simply fail on an expired token. Returns 0 when credentials
 * exist, -1 when the provider is logged out and none have reappeared. */
static int prepare_auth(struct codex *codex, int allow_refresh, http_tick_cb tick, void *tick_user)
{
    if (!codex->auth.access_token) {
        /* /logout cleared the live auth; a later /login — possibly in another hax process — or a
         * codex CLI login can restore it, but only for the pinned account: switching whose
         * account a conversation is sent under takes an explicit /login or /provider action. */
        codex_auth_load(&codex->auth, NULL);
        codex->account_blocked = 0;
        if (codex->auth.access_token && codex->account_pin && codex->auth.account_id &&
            strcmp(codex->auth.account_id, codex->account_pin) != 0) {
            codex_auth_release(&codex->auth);
            codex->account_blocked = 1;
        }
        return codex->auth.access_token ? 0 : -1;
    }

    if (codex->auth.source == CODEX_AUTH_SOURCE_HAX) {
        /* A failed proactive refresh is not terminal here: the request's own 401 recovery
         * reports it if the stale token really is rejected. */
        if (allow_refresh)
            codex_login_ensure_fresh(&codex->auth, 0, tick, tick_user);
    } else {
        reload_auth_if_stale(codex);
    }
    return 0;
}

/* One forced refresh per operation after a 401 on hax-owned credentials. Returns 1 when the
 * request should be retried with rebuilt headers. */
static int recover_unauthorized(struct codex *codex, int *auth_retried, http_tick_cb tick,
                                void *tick_user)
{
    if (*auth_retried || codex->auth.source != CODEX_AUTH_SOURCE_HAX)
        return 0;
    *auth_retried = 1;
    codex->refresh_transient = 0;
    switch (codex_login_ensure_fresh(&codex->auth, 1, tick, tick_user)) {
    case CODEX_REFRESH_FRESH:
        return 1;
    case CODEX_REFRESH_TRANSIENT:
        /* Credentials stay; the next attempt refreshes again. */
        codex->refresh_transient = 1;
        return 0;
    case CODEX_REFRESH_DEAD:
        break;
    }

    /* The managed login is dead or was removed by a concurrent /logout. Retry with what the
     * canonical load finds — possibly the codex CLI fallback — but never resend the failed
     * request across an account boundary: credentials outside the pinned account are left for
     * the user to adopt explicitly. Otherwise clear the live auth so later requests report the
     * logged-out state rather than resending the dead token. */
    struct codex_auth fallback;
    if (codex_auth_load(&fallback, NULL) == CODEX_AUTH_OK &&
        !codex_auth_equal(&fallback, &codex->auth) && fallback.account_id && codex->account_pin &&
        strcmp(fallback.account_id, codex->account_pin) == 0) {
        codex_auth_release(&codex->auth);
        codex->auth = fallback;
        return 1;
    }
    codex_auth_release(&fallback);
    codex_auth_release(&codex->auth);
    return 0;
}

static const char *token_expired_message(const struct codex *codex)
{
    if (codex->auth.source != CODEX_AUTH_SOURCE_HAX)
        return CODEX_TOKEN_EXPIRED_CLI;
    return codex->refresh_transient ? CODEX_TOKEN_REFRESH_FAILED : CODEX_TOKEN_EXPIRED_HAX;
}

static const char *not_logged_in_message(const struct codex *codex)
{
    return codex->account_blocked ? CODEX_ACCOUNT_CHANGED : CODEX_NOT_LOGGED_IN;
}

/* Record a request-level 401 so the next request re-evaluates borrowed credentials. */
static void note_unauthorized(struct codex *codex)
{
    if (codex->auth.source == CODEX_AUTH_SOURCE_CODEX_CLI)
        codex->auth_stale = 1;
}

static char *build_request_body(const struct context *context, const char *provider,
                                const char *model, const char *cache_key, const json_t *extra_body)
{
    json_t *body = responses_build_body(context, provider, model, NULL);
    json_object_set_new(body, "text", json_pack("{s:s}", "verbosity", "low"));
    if (cache_key)
        json_object_set_new(body, "prompt_cache_key", json_string(cache_key));
    provider_extra_body_apply(body, extra_body);

    char *body_json = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    return body_json;
}

static char **build_stream_headers(const struct codex *codex)
{
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
    return headers;
}

struct codex_stream {
    struct codex *codex;
    struct responses_events events;
    int auth_retried;
};

/* Tokens can rotate between attempts, so the auth headers are rebuilt for each. */
static char **stream_build_headers(void *ctx)
{
    return build_stream_headers(((struct codex_stream *)ctx)->codex);
}

static void stream_parser_init(void *ctx, stream_cb callback, void *callback_user)
{
    responses_events_init(&((struct codex_stream *)ctx)->events, callback, callback_user);
}

static int handle_sse_payload(const char *event_name, const char *data, void *user)
{
    (void)event_name;
    responses_events_feed(&((struct codex_stream *)user)->events, data);
    return 0;
}

static void stream_parser_finalize(void *ctx)
{
    responses_events_finalize(&((struct codex_stream *)ctx)->events);
}

static void stream_parser_free(void *ctx)
{
    responses_events_free(&((struct codex_stream *)ctx)->events);
}

static int stream_parser_complete(void *ctx)
{
    return ((struct codex_stream *)ctx)->events.terminal_emitted;
}

/* One in-place retry after a 401: adopt or refresh the rotated hax-owned token. */
static int stream_recover(void *ctx, long http_status, http_tick_cb tick, void *tick_user)
{
    struct codex_stream *stream = ctx;
    return http_status == 401 &&
           recover_unauthorized(stream->codex, &stream->auth_retried, tick, tick_user);
}

static char *stream_error_message(void *ctx, long http_status, const char *error_body)
{
    (void)error_body;
    if (http_status != 401)
        return NULL;
    struct codex *codex = ((struct codex_stream *)ctx)->codex;
    note_unauthorized(codex);
    return xstrdup(token_expired_message(codex));
}

static int codex_stream(struct provider *provider, const struct context *context, const char *model,
                        stream_cb callback, void *callback_user, http_tick_cb tick, void *tick_user)
{
    struct codex *codex = (struct codex *)provider;
    if (prepare_auth(codex, 1, tick, tick_user) != 0) {
        struct stream_event error_event = {
            .kind = EV_ERROR,
            .u.error = {.message = not_logged_in_message(codex), .http_status = 401},
        };
        callback(&error_event, callback_user);
        return -1;
    }

    if (codex->probe_deferred) {
        /* Non-blocking: relaunches the background metadata probe now that prepare_auth had its
         * refresh opportunity. A still-stale token just defers it again. */
        codex->probe_deferred = 0;
        model_meta_refresh(&codex->base, model);
    }

    char *body = build_request_body(context, provider_stable_id(&codex->base), model,
                                    codex->session_id, codex->extra_body);
    if (!body)
        return -1;

    struct codex_stream stream = {.codex = codex};
    struct stream_retry request = {
        .endpoint = CODEX_RESPONSES_ENDPOINT,
        .body = body,
        .body_len = strlen(body),
        .ctx = &stream,
        .build_headers = stream_build_headers,
        .parser_init = stream_parser_init,
        .parser_feed = handle_sse_payload,
        .parser_finalize = stream_parser_finalize,
        .parser_free = stream_parser_free,
        .parser_complete = stream_parser_complete,
        .recover = stream_recover,
        .error_message = stream_error_message,
    };
    int result = stream_retry_run(&request, callback, callback_user, tick, tick_user);
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

    if (prepare_auth(codex, 0, NULL, NULL) != 0)
        return -1;
    /* The probe only has to outlive its own short request, so its margin is much tighter than
     * the proactive-refresh window; a token the next request will rotate can still serve it. */
    if (codex->auth.source == CODEX_AUTH_SOURCE_HAX &&
        codex_login_token_expiring(codex->auth.access_token, CODEX_PROBE_TOKEN_MARGIN_S)) {
        codex->probe_deferred = 1;
        return -1;
    }
    probe->url =
        xasprintf("%s?client_version=%s", CODEX_MODELS_ENDPOINT, CODEX_MODEL_CLIENT_VERSION);
    probe->headers = build_model_headers(codex);
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

static int codex_query_usage(struct provider *provider)
{
    struct codex *codex = (struct codex *)provider;
    /* The busy scope opens before prepare_auth so a near-expiry token refresh shows the spinner
     * and honors Esc instead of freezing the command. */
    struct busy *busy = busy_begin("fetching usage...");
    if (prepare_auth(codex, 1, busy_tick, NULL) != 0) {
        if (!busy_end(busy))
            ui_error("%s", not_logged_in_message(codex));
        return -1;
    }
    char *body = NULL;
    long status = 0;
    int result = -1;
    int auth_retried = 0;
    do {
        free(body);
        body = NULL;
        char **headers = build_model_headers(codex);
        result = http_get(CODEX_USAGE_ENDPOINT, (const char *const *)headers,
                          CODEX_USAGE_TIMEOUT_SECONDS, 0, busy_tick, NULL, &body, &status);
        string_array_free(headers);
    } while (status == 401 && recover_unauthorized(codex, &auth_retried, busy_tick, NULL));
    int cancelled = busy_end(busy);
    if (cancelled) {
        free(body);
        return -1;
    }
    if (result != 0 || !body) {
        if (status == 401) {
            note_unauthorized(codex);
            ui_error("%s", token_expired_message(codex));
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
    if (codex->auth.email) {
        char *email = ctrl_strip_line_dup(codex->auth.email);
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

/* The provider-wide superset is narrowed by per-model catalog metadata before use. "ultra" is an
 * official-client policy label, not a reasoning value accepted by the API. */
static const char *const CODEX_EFFORTS[] = {"none", "low", "medium", "high", "xhigh", "max"};

static size_t codex_list_efforts(struct provider *provider, const char *const **efforts)
{
    (void)provider;
    *efforts = CODEX_EFFORTS;
    return sizeof(CODEX_EFFORTS) / sizeof(CODEX_EFFORTS[0]);
}

char *codex_model_catalog_error(long http_status, const char *token_expired)
{
    if (http_status == 401)
        return xstrdup(token_expired);
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
    if (prepare_auth(codex, 1, tick, tick_user) != 0) {
        *error = xstrdup(not_logged_in_message(codex));
        return -1;
    }

    char *url =
        xasprintf("%s?client_version=%s", CODEX_MODELS_ENDPOINT, CODEX_MODEL_CLIENT_VERSION);
    char *body = NULL;
    long http_status = 0;
    int result = -1;
    int auth_retried = 0;
    do {
        free(body);
        body = NULL;
        char **headers = build_model_headers(codex);
        result = http_get(url, (const char *const *)headers, CODEX_MODEL_TIMEOUT_SECONDS, 0, tick,
                          tick_user, &body, &http_status);
        string_array_free(headers);
    } while (http_status == 401 && recover_unauthorized(codex, &auth_retried, tick, tick_user));
    free(url);
    if (result != 0) {
        if (http_status == 401)
            note_unauthorized(codex);
        *error = codex_model_catalog_error(http_status, token_expired_message(codex));
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

void codex_provider_reload_auth(struct provider *provider)
{
    struct codex *codex = (struct codex *)provider;
    codex_auth_release(&codex->auth);
    /* When no credentials remain — /logout with no CLI fallback — the auth stays cleared so
     * requests report "not logged in" instead of continuing on the removed token. This explicit
     * action is also what may re-pin the provider to a different account. */
    codex_auth_load(&codex->auth, NULL);
    free(codex->account_pin);
    codex->account_pin = xstrdup(codex->auth.account_id);
    codex->account_blocked = 0;
    codex->auth_stale = 0;
}

static void codex_destroy(struct provider *provider)
{
    struct codex *codex = (struct codex *)provider;
    model_meta_release(provider);
    codex_auth_release(&codex->auth);
    free(codex->account_pin);
    free(codex->catalog_id);
    free(codex->name);
    free(codex->default_model);
    free(codex->default_effort);
    free(codex->session_id);
    string_array_free(codex->extra_headers);
    json_decref(codex->extra_body);
    free(codex);
}

struct provider *codex_provider_new(const struct provider_def *def)
{
    static const char *const EXTRA_FIELDS[] = {"display_name", "sort_models",   "catalog_id",
                                               "extra_body",   "extra_headers", NULL};
    provider_warn_unused_fields(def->id, NULL, 0, EXTRA_FIELDS);
    struct codex_auth auth;
    char *detail = NULL;
    enum codex_auth_status status = codex_auth_load(&auth, &detail);
    switch (status) {
    case CODEX_AUTH_OK:
        break;
    case CODEX_AUTH_NO_FILE:
        hax_err("no ChatGPT login found — run /login, or log in with the codex CLI (%s)", detail);
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
    codex->account_pin = xstrdup(auth.account_id);
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
    codex->base.id = def->id;
    /* Subscription responses report no cost, so estimate against equivalent OpenAI API rates
     * unless the block names another catalog identity (an empty value opts out). */
    const char *catalog_id = config_str("providers.codex.catalog_id");
    if (!catalog_id)
        catalog_id = "openai";
    codex->catalog_id = *catalog_id ? xstrdup(catalog_id) : NULL;
    codex->base.catalog_id = codex->catalog_id;
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
