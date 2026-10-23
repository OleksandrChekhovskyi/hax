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

/* A publisher model is named by its resource name; the chat surface takes the bare id. */
#define VERTEX_MODEL_PREFIX "publishers/"

/* Model Garden lists every family beside the chat models. The other families answer a different
 * request shape: embeddings, live sessions, and text-to-speech have no chat completion, and a
 * transcription model refuses one ("No multi-modal generation support in registered version"). */
static const char *const VERTEX_OTHER_FAMILIES[] = {
    "-embedding", "-tts", "-live", "-robotics", "-aqa", "-transcribe", NULL};

/* The publisher and the bare model id of a catalog entry, or zero when the entry names
 * something else. */
static int vertex_split_name(const json_t *entry, const struct vertex_publisher **publisher,
                             const char **model_id)
{
    const char *name = json_string_value(json_object_get(entry, "name"));
    if (!name)
        return 0;
    if (strncmp(name, VERTEX_MODEL_PREFIX, strlen(VERTEX_MODEL_PREFIX)) != 0)
        return 0;

    const char *rest = name + strlen(VERTEX_MODEL_PREFIX);
    const char *slash = strchr(rest, '/');
    if (!slash)
        return 0;
    size_t publisher_len = (size_t)(slash - rest);
    const char *after = slash + 1;
    if (strncmp(after, "models/", strlen("models/")) != 0)
        return 0;
    const char *id = after + strlen("models/");
    if (!*id)
        return 0;

    for (size_t i = 0; i < VERTEX_N_PUBLISHERS; i++) {
        const struct vertex_publisher *candidate = &VERTEX_PUBLISHERS[i];
        if (strlen(candidate->publisher) == publisher_len &&
            strncmp(candidate->publisher, rest, publisher_len) == 0) {
            *publisher = candidate;
            *model_id = id;
            return 1;
        }
    }
    return 0;
}

/* Whether the chat surface can serve this model. */
static int vertex_model_is_chat(const char *id, const char *stage)
{
    for (size_t i = 0; VERTEX_OTHER_FAMILIES[i]; i++) {
        if (strstr(id, VERTEX_OTHER_FAMILIES[i]))
            return 0;
    }
    /* Preview stages are limited to a few customers; most projects cannot call them. */
    if (stage && (strcmp(stage, "EXPERIMENTAL") == 0 || strcmp(stage, "PRIVATE_PREVIEW") == 0))
        return 0;
    return 1;
}

/* The id a user names the model by. A publisher whose models carry a version names it in the
 * id, the way the Claude endpoint does. */
static char *vertex_model_name(const struct vertex_publisher *publisher, const char *id,
                               const char *version)
{
    if (!publisher->versioned || !version || !*version)
        return xstrdup(id);
    return xasprintf("%s@%s", id, version);
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
        const struct vertex_publisher *publisher = NULL;
        const char *id = NULL;
        if (!vertex_split_name(entry, &publisher, &id))
            continue;
        /* A publisher serves several families, and only some of them answer a chat request. */
        if (strncmp(id, publisher->chat_prefix, strlen(publisher->chat_prefix)) != 0)
            continue;
        const char *stage = json_string_value(json_object_get(entry, "launchStage"));
        if (!vertex_model_is_chat(id, stage))
            continue;
        const char *version = json_string_value(json_object_get(entry, "versionId"));
        if (n_available == capacity) {
            capacity = capacity ? capacity * 2 : (n_entries > 8 ? n_entries : 8);
            available = xrealloc(available, capacity * sizeof(*available));
        }
        model_info_init(&available[n_available]);
        available[n_available].id = vertex_model_name(publisher, id, version);
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

/* One publisher's models, page by page. The entries move into the collected list. */
static int vertex_list_publisher(const char *origin, const char *publisher,
                                 const struct http_auth_source *auth, char *const *headers,
                                 struct model_info **available, size_t *n_available,
                                 size_t *capacity, char **error, http_tick_cb tick, void *tick_user)
{
    char *page_token = NULL;
    int result = 0;

    for (int page_number = 0; page_number < VERTEX_MODEL_PAGE_LIMIT; page_number++) {
        /* The service picks the page size; asking for one the catalog may not serve is a 400. */
        char *encoded = page_token ? url_encode(page_token) : NULL;
        char *url = encoded ? xasprintf("%s/v1beta1/publishers/%s/models?pageToken=%s", origin,
                                        publisher, encoded)
                            : xasprintf("%s/v1beta1/publishers/%s/models", origin, publisher);
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
            if (*n_available == *capacity) {
                *capacity = *capacity ? *capacity * 2 : 32;
                *available = xrealloc(*available, *capacity * sizeof(**available));
            }
            (*available)[(*n_available)++] = page_models[i];
        }
        free(page_models);

        free(page_token);
        page_token = next_token;
        if (!page_token)
            break;
    }

    free(page_token);
    return result;
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
    int result = 0;

    /* Each publisher keeps its own catalog: the Gemini models of one, the Claude models of
     * another. */
    for (size_t i = 0; i < VERTEX_N_PUBLISHERS; i++) {
        if (vertex_list_publisher(origin, VERTEX_PUBLISHERS[i].publisher, auth, headers, &available,
                                  &n_available, &capacity, error, tick, tick_user) != 0) {
            result = -1;
            break;
        }
    }

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
