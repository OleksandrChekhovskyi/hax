/* SPDX-License-Identifier: MIT */
#include "providers/openai_models.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "provider.h"
#include "xalloc.h"
#include "providers/http_provider.h"
#include "transport/api_error.h"
#include "transport/http.h"

#define MODEL_LIST_TIMEOUT_S 10

int openai_list_models(struct provider *provider, struct model_info **models, size_t *n_models,
                       char **error, http_tick_cb tick, void *tick_user)
{
    *models = NULL;
    *n_models = 0;

    const char *base_url = http_provider_base_url(provider);
    char *url = xasprintf("%s/models", base_url);
    char **headers = http_provider_metadata_headers(provider);

    char *response_body = NULL;
    long status = 0;
    int result = http_get(url, (const char *const *)headers, MODEL_LIST_TIMEOUT_S, 0, tick,
                          tick_user, &response_body, &status);
    string_array_free(headers);
    free(url);

    if (result != 0) {
        *error = format_model_list_error(provider->name, base_url,
                                         http_provider_has_api_key(provider), status);
        free(response_body);
        return -1;
    }

    json_t *root = json_loads(response_body, 0, NULL);
    free(response_body);
    const char *provider_name = provider->name ? provider->name : "provider";
    if (!root) {
        *error = xasprintf("%s /models response is not valid JSON", provider_name);
        return -1;
    }

    json_t *data = json_object_get(root, "data");
    /* Ollama reports data:null when the server is reachable but has no models. */
    if (json_is_null(data) || (json_is_array(data) && json_array_size(data) == 0)) {
        json_decref(root);
        return 0;
    }
    if (!json_is_array(data)) {
        json_decref(root);
        *error = xasprintf("%s /models response has no model list", provider_name);
        return -1;
    }

    http_parse_model_cb parse_model = http_provider_parse_model(provider);
    size_t n_entries = json_array_size(data);
    struct model_info *available = xmalloc(n_entries * sizeof(*available));
    size_t n_available = 0;
    for (size_t i = 0; i < n_entries; i++) {
        json_t *entry = json_array_get(data, i);
        const char *model_id = json_string_value(json_object_get(entry, "id"));
        if (!model_id || !*model_id)
            continue;

        model_info_init(&available[n_available]);
        available[n_available].id = xstrdup(model_id);
        if (parse_model)
            parse_model(entry, &available[n_available]);
        n_available++;
    }
    json_decref(root);

    if (n_available == 0) {
        free(available);
        *error = xasprintf("%s /models response contains no usable model ids", provider_name);
        return -1;
    }

    *models = available;
    *n_models = n_available;
    return 0;
}
