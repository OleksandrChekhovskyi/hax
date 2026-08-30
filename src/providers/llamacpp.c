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
#include "diag.h"
#include "provider.h"
#include "util.h"
#include "providers/http_provider.h"
#include "providers/provider_config.h"
#include "providers/registry.h"
#include "system/path.h"
#include "transport/http.h"

#define MODEL_LIST_TIMEOUT_S 2
/* A router autoloads the probed model before /props responds, so allow a full model load. */
#define MODEL_METADATA_TIMEOUT_S 120

static char *default_base_url(void)
{
    return xasprintf("http://127.0.0.1:%d/v1", config_int("providers.llamacpp.port"));
}

static char *resolve_base_url(void)
{
    char *default_url = default_base_url();
    const char *configured_url = config_str_nonempty("providers.llamacpp.base_url");
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

static int entry_names_model(const json_t *entry, const char *model)
{
    const char *id = json_string_value(json_object_get(entry, "id"));
    if (id && strcmp(id, model) == 0)
        return 1;
    json_t *aliases = json_object_get(entry, "aliases");
    size_t alias_count = json_is_array(aliases) ? json_array_size(aliases) : 0;
    for (size_t i = 0; i < alias_count; i++) {
        const char *alias = json_string_value(json_array_get(aliases, i));
        if (alias && strcmp(alias, model) == 0)
            return 1;
    }
    return 0;
}

int llamacpp_reconcile_model(const char *body, const char *configured_model,
                             struct llamacpp_reconcile *decision)
{
    memset(decision, 0, sizeof(*decision));
    json_t *root = json_loads(body, 0, NULL);
    json_t *models = root ? json_object_get(root, "data") : NULL;
    if (!json_is_array(models)) {
        json_decref(root);
        return -1;
    }

    int configured = configured_model && *configured_model;
    const char *first_model = NULL;
    const char *running_model = NULL;
    const char *configured_id = NULL;
    size_t running_count = 0;
    int router = 0;

    size_t model_count = json_array_size(models);
    for (size_t i = 0; i < model_count; i++) {
        json_t *entry = json_array_get(models, i);
        const char *served_model = json_string_value(json_object_get(entry, "id"));
        if (!served_model)
            continue;
        if (!first_model)
            first_model = served_model;
        json_t *status = json_object_get(entry, "status");
        if (json_is_object(status)) {
            router = 1;
            /* llama.cpp's running states: a loading model is committed and counts. */
            const char *state = json_string_value(json_object_get(status, "value"));
            if (state && (strcmp(state, "loaded") == 0 || strcmp(state, "loading") == 0 ||
                          strcmp(state, "sleeping") == 0)) {
                running_model = served_model;
                running_count++;
            }
        }
        if (configured && !configured_id && entry_names_model(entry, configured_model))
            configured_id = served_model;
    }

    if (!first_model) {
        decision->no_models = 1;
    } else if (!configured) {
        if (!router)
            decision->replacement = xstrdup(first_model);
        else if (running_count == 1)
            decision->replacement = xstrdup(running_model);
    } else if (!configured_id) {
        if (router)
            decision->clear_configured = 1;
        else
            decision->replacement = xstrdup(first_model);
    } else if (strcmp(configured_id, configured_model) != 0) {
        /* The picker and the session speak catalog ids, so normalize a configured alias. */
        decision->canonical = xstrdup(configured_id);
    }
    json_decref(root);
    return 0;
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

/* Server-discovered models are run overrides because the server may serve a different model on
 * the next launch. An explicit model is retained while the server is unreachable so the request
 * can report the underlying connection error. */
static int reconcile_configured_model(const char *base_url, const char *api_key,
                                      int *model_discovered)
{
    *model_discovered = 0;
    char *url = xasprintf("%s/models", base_url);
    char *authorization = api_key ? xasprintf("Authorization: Bearer %s", api_key) : NULL;
    const char *fixed[] = {authorization, NULL};
    char **extra_headers = provider_extra_headers("providers.llamacpp");
    char **headers = string_array_concat(fixed, (const char *const *)extra_headers);
    string_array_free(extra_headers);
    free(authorization);
    char *body = NULL;
    int request_succeeded = http_get(url, (const char *const *)headers, MODEL_LIST_TIMEOUT_S, 0,
                                     NULL, NULL, &body, NULL) == 0;
    string_array_free(headers);

    const char *configured_model = config_str("model");
    int configured = configured_model && *configured_model;
    int result = -1;
    struct llamacpp_reconcile decision;
    if (request_succeeded && llamacpp_reconcile_model(body, configured_model, &decision) == 0) {
        if (decision.no_models) {
            hax_warn("llama.cpp: no models available — start llama-server with -m, -hf, or "
                     "--models-dir");
            if (configured)
                config_set_override("model", "");
        } else if (decision.replacement) {
            if (configured) {
                char *warning = llamacpp_model_warning(configured_model, decision.replacement);
                hax_warn("%s", warning);
                free(warning);
            }
            config_set_override("model", decision.replacement);
            *model_discovered = 1;
        } else if (decision.canonical) {
            config_set_override("model", decision.canonical);
        } else if (decision.clear_configured) {
            hax_warn("llama.cpp: model '%s' is not in the router catalog — pick one with /model",
                     configured_model);
            config_set_override("model", "");
        }
        free(decision.canonical);
        free(decision.replacement);
        result = 0;
    } else if (!request_succeeded && configured) {
        result = 0;
    }

    free(body);
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

void llamacpp_parse_model(const json_t *entry, struct model_info *info)
{
    json_t *meta = json_object_get(entry, "meta");
    json_t *context = meta ? json_object_get(meta, "n_ctx") : NULL;
    if (json_is_integer(context) && json_integer_value(context) > 0)
        info->context = (long)json_integer_value(context);

    json_t *architecture = json_object_get(entry, "architecture");
    json_t *modalities = architecture ? json_object_get(architecture, "input_modalities") : NULL;
    if (json_is_array(modalities)) {
        info->image_input = PROVIDER_CAP_NO;
        size_t modality_count = json_array_size(modalities);
        for (size_t i = 0; i < modality_count; i++) {
            const char *modality = json_string_value(json_array_get(modalities, i));
            if (modality && strcmp(modality, "image") == 0)
                info->image_input = PROVIDER_CAP_YES;
        }
    }

    /* Most router models sit unloaded; flag only the exceptions. A failed load reports as
     * unloaded plus a failed marker and exit code. */
    json_t *status = json_object_get(entry, "status");
    const char *state = status ? json_string_value(json_object_get(status, "value")) : NULL;
    if (status && json_is_true(json_object_get(status, "failed"))) {
        json_t *exit_code = json_object_get(status, "exit_code");
        info->description =
            json_is_integer(exit_code)
                ? xasprintf("failed (exit %lld)", (long long)json_integer_value(exit_code))
                : xstrdup("failed");
    } else if (state && strcmp(state, "unloaded") != 0) {
        info->description = xstrdup(state);
    }
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

int llamacpp_probe_model(struct provider *provider, const char *model, struct model_probe *probe)
{
    (void)provider;
    char *base_url = resolve_base_url();
    probe->url = llamacpp_props_url(base_url, model);
    free(base_url);
    if (!probe->url)
        return -1;

    const char *api_key = provider_api_key("providers.llamacpp", NULL);
    char *authorization = api_key ? xasprintf("Authorization: Bearer %s", api_key) : NULL;
    const char *fixed[] = {authorization, NULL};
    char **extra_headers = provider_extra_headers("providers.llamacpp");
    probe->headers = string_array_concat(fixed, (const char *const *)extra_headers);
    string_array_free(extra_headers);
    free(authorization);
    probe->timeout_s = MODEL_METADATA_TIMEOUT_S;
    probe->parse = parse_props;
    return 0;
}

int llamacpp_discover(const char *base_url, int *model_discovered)
{
    const char *api_key = provider_api_key("providers.llamacpp", NULL);
    if (reconcile_configured_model(base_url, api_key, model_discovered) != 0) {
        hax_err("llama.cpp: failed to auto-discover model from %s/models\n"
                "hax: is llama-server running? "
                "(set HAX_MODEL to skip probing, or adjust HAX_LLAMACPP_PORT / "
                "HAX_LLAMACPP_BASE_URL)",
                base_url);
        return -1;
    }
    return 0;
}

void llamacpp_prepare_availability(const struct provider_def *def,
                                   struct provider_availability *out)
{
    (void)def;
    char *base_url = resolve_base_url();
    char **extra_headers = provider_extra_headers("providers.llamacpp");
    http_provider_prepare_base_url_availability(
        base_url, provider_api_key("providers.llamacpp", NULL), extra_headers, out);
    string_array_free(extra_headers);
    free(base_url);
}
