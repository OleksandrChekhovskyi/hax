/* SPDX-License-Identifier: MIT */
#include "providers/vertex_models.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "provider.h"
#include "xalloc.h"
#include "providers/http_provider.h"
#include "providers/vertex.h"
#include "text/url.h"
#include "transport/http.h"

/* The publisher catalog is paged; a page holds one slice of the models. */
#define VERTEX_MODEL_PAGE_LIMIT 20
#define VERTEX_MODEL_TIMEOUT_S  15

/* Publisher models are named by their resource name; the chat surface takes the bare id. */
#define VERTEX_MODEL_PREFIX "publishers/google/models/"

/* Model Garden lists every family beside the chat models, and the OpenAI-compatible surface
 * serves the Gemini chat ones. The other families answer a different request shape: embeddings,
 * live sessions, and text-to-speech have no chat completion, and a transcription model refuses
 * one ("No multi-modal generation support in registered version"). */
static const char *const VERTEX_OTHER_FAMILIES[] = {
    "-embedding", "-tts", "-live", "-robotics", "-aqa", "-transcribe", NULL};

/* The bare model id of a publisher entry, or NULL when the entry names something else. */
static char *vertex_model_id(const json_t *entry)
{
    const char *name = json_string_value(json_object_get(entry, "name"));
    if (!name)
        return NULL;
    size_t prefix_len = strlen(VERTEX_MODEL_PREFIX);
    if (strncmp(name, VERTEX_MODEL_PREFIX, prefix_len) != 0)
        return NULL;
    const char *id = name + prefix_len;
    return *id ? xstrdup(id) : NULL;
}

/* Whether the chat surface can serve this model. */
static int vertex_model_is_chat(const char *id, const char *stage)
{
    if (strncmp(id, "gemini-", 7) != 0)
        return 0;
    for (size_t i = 0; VERTEX_OTHER_FAMILIES[i]; i++) {
        if (strstr(id, VERTEX_OTHER_FAMILIES[i]))
            return 0;
    }
    /* Preview stages are limited to a few customers; most projects cannot call them. */
    if (stage && (strcmp(stage, "EXPERIMENTAL") == 0 || strcmp(stage, "PRIVATE_PREVIEW") == 0))
        return 0;
    return 1;
}

/* A note for the picker: a preview model behaves differently from a released one. */
static char *vertex_stage_note(const char *stage)
{
    if (stage && strcmp(stage, "PUBLIC_PREVIEW") == 0)
        return xstrdup("public preview");
    return NULL;
}

int vertex_parse_publisher_models(const char *body, struct model_info **models, size_t *n_models,
                                  char **next_page_token, char **error)
{
    *models = NULL;
    *n_models = 0;
    *next_page_token = NULL;

    json_t *root = json_loads(body, 0, NULL);
    if (!root) {
        *error = xstrdup("Vertex AI model list response is not valid JSON");
        return -1;
    }
    json_t *entries = json_object_get(root, "publisherModels");
    if (!json_is_array(entries)) {
        json_decref(root);
        *error = xstrdup("Vertex AI model list response has no model list");
        return -1;
    }

    struct model_info *available = NULL;
    size_t n_available = 0;
    size_t capacity = 0;
    size_t n_entries = json_array_size(entries);
    for (size_t i = 0; i < n_entries; i++) {
        json_t *entry = json_array_get(entries, i);
        char *id = vertex_model_id(entry);
        if (!id)
            continue;
        const char *stage = json_string_value(json_object_get(entry, "launchStage"));
        if (!vertex_model_is_chat(id, stage)) {
            free(id);
            continue;
        }
        if (n_available == capacity) {
            capacity = capacity ? capacity * 2 : (n_entries > 8 ? n_entries : 8);
            available = xrealloc(available, capacity * sizeof(*available));
        }
        model_info_init(&available[n_available]);
        available[n_available].id = id;
        available[n_available].description = vertex_stage_note(stage);
        n_available++;
    }

    const char *token = json_string_value(json_object_get(root, "nextPageToken"));
    if (token && *token)
        *next_page_token = xstrdup(token);
    json_decref(root);

    *models = available;
    *n_models = n_available;
    return 0;
}

/* A failed listing says what the endpoint answered; an unauthorized one names the credentials. */
static char *vertex_list_error(const struct http_auth_source *auth, long status)
{
    if (status == 401)
        return auth->ops ? auth->ops->unauthorized_message(auth->state)
                         : xstrdup("Vertex AI credentials were rejected");
    if (status == 403)
        return xstrdup("Vertex AI refused the request (HTTP 403, the project may need the "
                       "Vertex AI API enabled, or the credential may carry no quota project)");
    if (status >= 200 && status < 300)
        return xstrdup("Vertex AI model list response was empty");
    if (status > 0)
        return xasprintf("listing Vertex AI models failed (HTTP %ld)", status);
    return xstrdup("could not reach the Vertex AI model list");
}

int vertex_list_models(struct provider *provider, struct model_info **models, size_t *n_models,
                       char **error, http_tick_cb tick, void *tick_user)
{
    *models = NULL;
    *n_models = 0;

    char *origin = vertex_api_origin();
    if (!origin) {
        *error = xstrdup("Vertex AI endpoint is not configured");
        return -1;
    }

    const struct http_auth_source *auth = http_provider_auth(provider);
    if (!auth->ops || auth->ops->prepare(auth->state, 1, tick, tick_user) != 0) {
        *error = auth->ops ? auth->ops->unauthorized_message(auth->state)
                           : xstrdup("Vertex AI credentials are not configured");
        free(origin);
        return -1;
    }

    char **headers = http_provider_metadata_headers(provider);
    /* The publisher API carries no project in its path, so a credential without a quota project
     * is refused outright: name the project the calls belong to. */
    char *project = vertex_project_id();
    if (project && *project) {
        char *quota = xasprintf("x-goog-user-project: %s", project);
        const char *extra[] = {quota, NULL};
        char **with_quota = string_array_concat((const char *const *)headers, extra);
        string_array_free(headers);
        headers = with_quota;
        free(quota);
    }
    free(project);
    struct model_info *available = NULL;
    size_t n_available = 0;
    size_t capacity = 0;
    char *page_token = NULL;
    int result = 0;

    for (int page_number = 0; page_number < VERTEX_MODEL_PAGE_LIMIT; page_number++) {
        /* The service picks the page size; asking for one the catalog may not serve is a 400. */
        char *encoded = page_token ? url_encode(page_token) : NULL;
        char *url =
            encoded ? xasprintf("%s/v1beta1/publishers/google/models?pageToken=%s", origin, encoded)
                    : xasprintf("%s/v1beta1/publishers/google/models", origin);
        free(encoded);

        char *body = NULL;
        long status = 0;
        int request = http_get(url, (const char *const *)headers, VERTEX_MODEL_TIMEOUT_S, 0, tick,
                               tick_user, &body, &status);
        free(url);
        if (request != 0) {
            *error = vertex_list_error(auth, status);
            result = -1;
            break;
        }

        struct model_info *page_models = NULL;
        size_t n_page_models = 0;
        char *next_token = NULL;
        if (vertex_parse_publisher_models(body, &page_models, &n_page_models, &next_token, error) !=
            0) {
            free(body);
            result = -1;
            break;
        }
        free(body);

        /* The entries move into the collected list, so only their array is released here. */
        for (size_t i = 0; i < n_page_models; i++) {
            if (n_available == capacity) {
                capacity = capacity ? capacity * 2 : 32;
                available = xrealloc(available, capacity * sizeof(*available));
            }
            available[n_available++] = page_models[i];
        }
        free(page_models);

        free(page_token);
        page_token = next_token;
        if (!page_token)
            break;
    }

    free(page_token);
    string_array_free(headers);
    free(origin);

    if (result != 0) {
        model_info_free(available, n_available);
        return -1;
    }

    *models = available;
    *n_models = n_available;
    return 0;
}
