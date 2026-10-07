/* SPDX-License-Identifier: MIT */
#include "providers/openai_models.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "provider.h"
#include "xalloc.h"
#include "providers/http_provider.h"
#include "text/json_scan.h"
#include "transport/api_error.h"
#include "transport/http.h"

#define MODEL_LIST_TIMEOUT_S  10
#define MODEL_PROBE_TIMEOUT_S 5

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

    /* OpenRouter's listing tree-parses to several megabytes, so parse one entry at a time. */
    const char *provider_name = provider->name ? provider->name : "provider";
    struct json_scan_entry data;
    int found = json_scan_find(response_body, "data", &data);
    /* Ollama reports data:null when the server is reachable but has no models. */
    if (found == 1 && data.value_len == 4 && memcmp(data.value, "null", 4) == 0) {
        free(response_body);
        return 0;
    }
    struct json_scan entries;
    if (found != 1 || json_scan_array(&entries, data.value) != 0) {
        free(response_body);
        *error = found < 0 ? xasprintf("%s /models response is not valid JSON", provider_name)
                           : xasprintf("%s /models response has no model list", provider_name);
        return -1;
    }

    http_parse_model_cb parse_model = http_provider_parse_model(provider);
    struct model_info *available = NULL;
    size_t n_available = 0;
    size_t capacity = 0;
    size_t n_entries = 0;
    struct json_scan_entry element;
    int scanned;
    while ((scanned = json_scan_next(&entries, &element)) == 1) {
        json_t *entry = json_scan_load(&element);
        if (!entry) {
            scanned = -1;
            break;
        }
        n_entries++;
        const char *model_id = json_string_value(json_object_get(entry, "id"));
        if (model_id && *model_id) {
            if (n_available == capacity) {
                capacity = capacity ? capacity * 2 : 64;
                available = xrealloc(available, capacity * sizeof(*available));
            }
            model_info_init(&available[n_available]);
            available[n_available].id = xstrdup(model_id);
            if (parse_model)
                parse_model(entry, &available[n_available]);
            n_available++;
        }
        json_decref(entry);
    }
    free(response_body);

    if (scanned < 0) {
        model_info_free(available, n_available);
        *error = xasprintf("%s /models response is not valid JSON", provider_name);
        return -1;
    }
    if (n_entries == 0)
        return 0;
    if (n_available == 0) {
        free(available);
        *error = xasprintf("%s /models response contains no usable model ids", provider_name);
        return -1;
    }

    *models = available;
    *n_models = n_available;
    return 0;
}

int openai_probe_model(struct provider *provider, const char *model, struct model_probe *probe)
{
    http_parse_model_cb parse_model = http_provider_parse_model(provider);
    if (!parse_model || !model || !*model)
        return -1;

    probe->url = xasprintf("%s/models", http_provider_base_url(provider));
    probe->headers = http_provider_metadata_headers(provider);
    probe->timeout_s = MODEL_PROBE_TIMEOUT_S;
    probe->parse_entry = parse_model;
    return 0;
}
