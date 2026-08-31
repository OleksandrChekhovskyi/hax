/* SPDX-License-Identifier: MIT */
#include "providers/anthropic_models.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "effort.h"
#include "provider.h"
#include "xalloc.h"
#include "providers/http_provider.h"
#include "transport/api_error.h"
#include "transport/http.h"

/* Bound foreground pagination if a server keeps returning advancing cursors. */
#define ANTHROPIC_MODEL_PAGE_SIZE  1000
#define ANTHROPIC_MODEL_PAGE_LIMIT 50

#define MODEL_LIST_TIMEOUT_S  10
#define MODEL_PROBE_TIMEOUT_S 5

static void parse_model_efforts(json_t *effort, struct effort_set *out)
{
    if (json_is_false(json_object_get(effort, "supported"))) {
        out->known = 1;
        return;
    }
    if (!json_is_object(effort))
        return;

    /* Preserve the ladder's picker order rather than the JSON object's member order. */
    for (size_t i = 0; i < EFFORT_LADDER_N; i++) {
        json_t *level = json_object_get(effort, EFFORT_LADDER[i]);
        if (json_is_object(level) && json_is_true(json_object_get(level, "supported")))
            effort_set_add(out, EFFORT_LADDER[i]);
    }

    const char *name;
    json_t *level;
    json_object_foreach(effort, name, level)
    {
        if (strcmp(name, "supported") != 0 && json_is_object(level) &&
            json_is_true(json_object_get(level, "supported"))) {
            effort_set_add(out, name);
        }
    }
}

void anthropic_parse_model(const json_t *entry, struct model_info *out)
{
    json_t *max_input = json_object_get(entry, "max_input_tokens");
    if (json_is_integer(max_input) && json_integer_value(max_input) > 0)
        out->context = (long)json_integer_value(max_input);

    json_t *max_output = json_object_get(entry, "max_tokens");
    if (json_is_integer(max_output) && json_integer_value(max_output) > 0)
        out->max_output = (long)json_integer_value(max_output);

    json_t *capabilities = json_object_get(entry, "capabilities");
    json_t *image = json_object_get(capabilities, "image_input");
    json_t *image_supported = json_object_get(image, "supported");
    if (json_is_boolean(image_supported)) {
        out->image_input = json_is_true(image_supported) ? PROVIDER_CAP_YES : PROVIDER_CAP_NO;
    }

    parse_model_efforts(json_object_get(capabilities, "effort"), &out->efforts);
}

static void parse_model_probe_response(const char *response_body, const char *model,
                                       struct model_info *out)
{
    json_t *root = json_loads(response_body, 0, NULL);
    if (!root)
        return;

    json_t *data = json_object_get(root, "data");
    size_t index;
    json_t *entry;
    json_array_foreach(data, index, entry)
    {
        const char *model_id = json_string_value(json_object_get(entry, "id"));
        if (model_id && strcmp(model_id, model) == 0) {
            anthropic_parse_model(entry, out);
            break;
        }
    }
    json_decref(root);
}

int anthropic_probe_model(struct provider *provider, const char *model, struct model_probe *probe)
{
    if (!model || !*model)
        return -1;

    probe->url = xasprintf("%s/models?limit=%d", http_provider_base_url(provider),
                           ANTHROPIC_MODEL_PAGE_SIZE);
    probe->headers = http_provider_metadata_headers(provider);
    probe->timeout_s = MODEL_PROBE_TIMEOUT_S;
    probe->parse = parse_model_probe_response;
    return 0;
}

/* The server-provided cursor is inserted into a query parameter without encoding. */
static int cursor_is_safe(const char *cursor)
{
    if (!cursor || !*cursor)
        return 0;
    for (const char *byte = cursor; *byte; byte++) {
        int safe = (*byte >= 'A' && *byte <= 'Z') || (*byte >= 'a' && *byte <= 'z') ||
                   (*byte >= '0' && *byte <= '9') || *byte == '.' || *byte == '_' || *byte == '-';
        if (!safe)
            return 0;
    }
    return 1;
}

/* On success, `page` owns the parsed response. */
static int fetch_models_page(struct provider *provider, const char *after_id, http_tick_cb tick,
                             void *tick_user, json_t **page, char **error)
{
    const char *base_url = http_provider_base_url(provider);
    char *url = after_id ? xasprintf("%s/models?limit=%d&after_id=%s", base_url,
                                     ANTHROPIC_MODEL_PAGE_SIZE, after_id)
                         : xasprintf("%s/models?limit=%d", base_url, ANTHROPIC_MODEL_PAGE_SIZE);
    char **request_headers = http_provider_metadata_headers(provider);

    char *response_body = NULL;
    long status = 0;
    int result = http_get(url, (const char *const *)request_headers, MODEL_LIST_TIMEOUT_S, 0, tick,
                          tick_user, &response_body, &status);
    string_array_free(request_headers);
    free(url);

    if (result != 0) {
        *error = format_model_list_error(provider->name, base_url,
                                         http_provider_has_api_key(provider), status);
        free(response_body);
        return -1;
    }

    *page = json_loads(response_body, 0, NULL);
    free(response_body);
    if (!*page) {
        const char *provider_name = provider->name ? provider->name : "provider";
        *error = xasprintf("%s /models response is not valid JSON", provider_name);
        return -1;
    }
    return 0;
}

static void append_models(json_t *data, struct model_info **models, size_t *n_models,
                          size_t *capacity, int *saw_entry)
{
    size_t n_entries = json_array_size(data);
    for (size_t i = 0; i < n_entries; i++) {
        json_t *entry = json_array_get(data, i);
        const char *model_id = json_string_value(json_object_get(entry, "id"));
        *saw_entry = 1;
        if (!model_id || !*model_id)
            continue;

        if (*n_models == *capacity) {
            *capacity = *capacity ? *capacity * 2 : (n_entries > 8 ? n_entries : 8);
            *models = xrealloc(*models, *capacity * sizeof(**models));
        }
        model_info_init(&(*models)[*n_models]);
        (*models)[*n_models].id = xstrdup(model_id);
        anthropic_parse_model(entry, &(*models)[*n_models]);
        (*n_models)++;
    }
}

int anthropic_list_models(struct provider *provider, struct model_info **models, size_t *n_models,
                          char **error, http_tick_cb tick, void *tick_user)
{
    *models = NULL;
    *n_models = 0;

    struct model_info *available = NULL;
    size_t n_available = 0;
    size_t capacity = 0;
    char *after_id = NULL;
    int saw_entry = 0;

    for (int page_number = 0; page_number < ANTHROPIC_MODEL_PAGE_LIMIT; page_number++) {
        json_t *page = NULL;
        if (fetch_models_page(provider, after_id, tick, tick_user, &page, error) != 0)
            goto fail;

        json_t *data = json_object_get(page, "data");
        if (!json_is_array(data) && !json_is_null(data)) {
            json_decref(page);
            const char *name = provider->name ? provider->name : "provider";
            *error = xasprintf("%s /models response has no model list", name);
            goto fail;
        }

        const char *last_id = json_string_value(json_object_get(page, "last_id"));
        /* Discard a repeated page before it can duplicate models already collected. */
        if (after_id && last_id && strcmp(after_id, last_id) == 0) {
            json_decref(page);
            break;
        }

        append_models(data, &available, &n_available, &capacity, &saw_entry);
        int has_more = json_is_true(json_object_get(page, "has_more"));
        free(after_id);
        after_id = has_more && cursor_is_safe(last_id) ? xstrdup(last_id) : NULL;
        json_decref(page);
        if (!after_id)
            break;
    }
    free(after_id);

    if (saw_entry && n_available == 0) {
        const char *name = provider->name ? provider->name : "provider";
        *error = xasprintf("%s /models response contains no usable model ids", name);
        model_info_free(available, n_available);
        return -1;
    }

    *models = available;
    *n_models = n_available;
    return 0;

fail:
    free(after_id);
    model_info_free(available, n_available);
    return -1;
}
