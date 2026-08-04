/* SPDX-License-Identifier: MIT */
#include "providers/llamacpp.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <curl/curl.h>
#include <curl/urlapi.h>

#include "config.h"
#include "model_meta.h"
#include "provider.h"
#include "util.h"
#include "providers/openai.h"
#include "transport/http.h"

#define MODEL_LIST_TIMEOUT_S     2
#define MODEL_METADATA_TIMEOUT_S 5

static char *default_base_url(void)
{
    return xasprintf("http://127.0.0.1:%d/v1", config_int("llamacpp.port"));
}

static char *resolve_base_url(void)
{
    char *default_url = default_base_url();
    const char *configured_url = config_str_nonempty("openai.base_url");
    char *base_url = dup_trim_trailing_slash(configured_url ? configured_url : default_url);
    free(default_url);
    return base_url;
}

static char *replace_url_path(const char *url, const char *path)
{
    CURLU *parsed_url = curl_url();
    if (!parsed_url)
        return NULL;

    char *result = NULL;
    if (curl_url_set(parsed_url, CURLUPART_URL, url, 0) == CURLUE_OK &&
        curl_url_set(parsed_url, CURLUPART_PATH, path, 0) == CURLUE_OK) {
        char *curl_url_string = NULL;
        if (curl_url_get(parsed_url, CURLUPART_URL, &curl_url_string, 0) == CURLUE_OK) {
            result = xstrdup(curl_url_string);
            curl_free(curl_url_string);
        }
    }
    curl_url_cleanup(parsed_url);
    return result;
}

char *llamacpp_props_url(const char *base_url, const char *model)
{
    char *url = replace_url_path(base_url, "/props");
    if (!url || !model || !*model)
        return url;

    CURLU *parsed_url = curl_url();
    if (!parsed_url)
        return url;

    char *query = xasprintf("model=%s", model);
    char *result = url;
    if (curl_url_set(parsed_url, CURLUPART_URL, url, 0) == CURLUE_OK &&
        curl_url_set(parsed_url, CURLUPART_QUERY, query, CURLU_APPENDQUERY | CURLU_URLENCODE) ==
            CURLUE_OK) {
        char *curl_url_string = NULL;
        if (curl_url_get(parsed_url, CURLUPART_URL, &curl_url_string, 0) == CURLUE_OK) {
            result = xstrdup(curl_url_string);
            curl_free(curl_url_string);
            free(url);
        }
    }
    free(query);
    curl_url_cleanup(parsed_url);
    return result;
}

int llamacpp_reconcile_model(const char *body, const char *configured_model, char **replacement)
{
    *replacement = NULL;
    json_t *root = json_loads(body, 0, NULL);
    json_t *models = root ? json_object_get(root, "data") : NULL;
    const char *first_served_model = NULL;
    int configured_model_is_served = 0;

    if (json_is_array(models)) {
        size_t model_count = json_array_size(models);
        for (size_t i = 0; i < model_count; i++) {
            const char *served_model =
                json_string_value(json_object_get(json_array_get(models, i), "id"));
            if (!served_model)
                continue;
            if (!first_served_model)
                first_served_model = served_model;
            if (configured_model && *configured_model &&
                strcmp(served_model, configured_model) == 0)
                configured_model_is_served = 1;
        }
    }

    int result = first_served_model ? 0 : -1;
    if (first_served_model &&
        (!configured_model || !*configured_model || !configured_model_is_served))
        *replacement = xstrdup(first_served_model);
    json_decref(root);
    return result;
}

char *llamacpp_model_warning(const char *configured_model, const char *served_model)
{
    char *configured_label = llamacpp_model_label(NULL, configured_model);
    char *served_label = llamacpp_model_label(NULL, served_model);
    char *warning;
    if (strcmp(configured_label, served_label) == 0)
        warning = xasprintf("llama.cpp: configured model is not served — using '%s'", served_label);
    else
        warning = xasprintf("llama.cpp: model '%s' is not served — using '%s'", configured_label,
                            served_label);
    free(served_label);
    free(configured_label);
    return warning;
}

/* Server-discovered models are run overrides because llama-server may serve a different model on
 * the next launch. An explicit model is retained while the server is unreachable so the request can
 * report the underlying connection error. */
static int reconcile_configured_model(const char *base_url, const char *api_key,
                                      int *model_discovered)
{
    *model_discovered = 0;
    char *url = xasprintf("%s/models", base_url);
    char *authorization = api_key ? xasprintf("Authorization: Bearer %s", api_key) : NULL;
    const char *headers[] = {authorization, NULL};
    char *body = NULL;
    int request_succeeded = http_get(url, authorization ? headers : NULL, MODEL_LIST_TIMEOUT_S, 0,
                                     NULL, NULL, &body, NULL) == 0;

    const char *configured_model = config_str("model");
    int result = -1;
    if (request_succeeded) {
        char *replacement = NULL;
        if (llamacpp_reconcile_model(body, configured_model, &replacement) == 0) {
            if (replacement) {
                if (configured_model && *configured_model) {
                    char *warning = llamacpp_model_warning(configured_model, replacement);
                    hax_warn("%s", warning);
                    free(warning);
                }
                config_set_override("model", replacement);
                free(replacement);
                *model_discovered = 1;
            }
            result = 0;
        }
    } else if (configured_model && *configured_model) {
        result = 0;
    }

    free(body);
    free(authorization);
    free(url);
    return result;
}

/* default_generation_settings.n_ctx is llama-server's stable runtime context-window field. Vision
 * support depends on the loaded mmproj projector and cannot come from a model catalog. Older
 * servers omit these fields, leaving the capabilities unknown. */
static void parse_props(const char *body, const char *model, struct model_info *model_info)
{
    (void)model;
    json_t *root = json_loads(body, 0, NULL);
    if (!root)
        return;

    json_t *settings = json_object_get(root, "default_generation_settings");
    json_t *context = settings ? json_object_get(settings, "n_ctx") : NULL;
    if (json_is_integer(context) && json_integer_value(context) > 0)
        model_info->context = (long)json_integer_value(context);

    json_t *modalities = json_object_get(root, "modalities");
    json_t *vision = modalities ? json_object_get(modalities, "vision") : NULL;
    if (json_is_boolean(vision))
        model_info->image_input = json_is_true(vision) ? PROVIDER_CAP_YES : PROVIDER_CAP_NO;
    json_decref(root);
}

char *llamacpp_model_label(struct provider *provider, const char *model)
{
    static const char GGUF_EXTENSION[] = ".gguf";
    (void)provider;

    size_t model_length = strlen(model);
    size_t extension_length = sizeof(GGUF_EXTENSION) - 1;
    if (model_length <= extension_length ||
        strcasecmp(model + model_length - extension_length, GGUF_EXTENSION) != 0)
        return xstrdup(model);

    const char *filename = strrchr(model, '/');
    const char *backslash = strrchr(model, '\\');
    if (!filename || (backslash && backslash > filename))
        filename = backslash;
    filename = filename ? filename + 1 : model;

    size_t stem_length = (size_t)(model + model_length - extension_length - filename);
    if (stem_length == 0)
        return xstrdup(model);
    char *label = xmalloc(stem_length + 1);
    memcpy(label, filename, stem_length);
    label[stem_length] = '\0';
    return label;
}

static int llamacpp_probe_model(struct provider *provider, const char *model,
                                struct model_probe *probe)
{
    (void)provider;
    char *base_url = resolve_base_url();
    probe->url = llamacpp_props_url(base_url, model);
    free(base_url);
    if (!probe->url)
        return -1;

    const char *api_key = config_str_nonempty("openai.api_key");
    if (api_key) {
        probe->headers = xcalloc(2, sizeof(*probe->headers));
        probe->headers[0] = xasprintf("Authorization: Bearer %s", api_key);
    }
    probe->timeout_s = MODEL_METADATA_TIMEOUT_S;
    probe->parse = parse_props;
    return 0;
}

struct provider *llamacpp_provider_new(const char *name)
{
    (void)name;
    char *default_url = default_base_url();
    char *base_url = resolve_base_url();
    const char *api_key = config_str_nonempty("openai.api_key");
    int model_discovered = 0;
    if (reconcile_configured_model(base_url, api_key, &model_discovered) != 0) {
        hax_err("llama.cpp: failed to auto-discover model from %s/models\n"
                "hax: is llama-server running? "
                "(set HAX_MODEL to skip probing, or adjust HAX_LLAMACPP_PORT / "
                "HAX_OPENAI_BASE_URL)",
                base_url);
        free(base_url);
        free(default_url);
        return NULL;
    }

    struct openai_preset preset = {
        .display_name = "llama.cpp",
        .default_base_url = default_url,
        .send_cache_key_default = 0,
        .emit_progress = 1,
        /* Interleaved-thinking models can leak tool calls into reasoning unless prior reasoning is
         * returned through llama-server's reasoning_content field. */
        .reasoning_replay_field = "reasoning_content",
        /* llama-server has no per-request context-size control. */
        .length_hint = "llama-server's context is full — restart it with a larger "
                       "-c / --ctx-size",
    };
    struct provider *provider = openai_provider_new_preset(&preset);
    if (provider) {
        provider->model_label = llamacpp_model_label;
        provider->probe_model = llamacpp_probe_model;
        provider->model_discovered = model_discovered;
        model_meta_refresh(provider, config_str("model"));
    }
    free(base_url);
    free(default_url);
    return provider;
}

static void llamacpp_prepare_availability(const char *name,
                                          struct provider_availability *availability)
{
    (void)name;
    char *base_url = resolve_base_url();
    openai_prepare_base_url_availability(base_url, config_str_nonempty("openai.api_key"),
                                         availability);
    free(base_url);
}

const struct provider_factory PROVIDER_LLAMACPP = {
    .name = "llama.cpp",
    .new = llamacpp_provider_new,
    .prepare_availability = llamacpp_prepare_availability,
};
