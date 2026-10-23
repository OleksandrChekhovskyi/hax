/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_VERTEX_MODELS_H
#define HAX_PROVIDERS_VERTEX_MODELS_H

#include <stddef.h>

#include "provider.h"

/* List the Gemini chat models of the Vertex publisher catalog. The OpenAI-compatible endpoint
 * serves no model list, so the listing comes from the native publisher API. */
int vertex_list_models(struct provider *provider, struct model_info **models, size_t *n_models,
                       char **error, http_tick_cb tick, void *tick_user);

/* Parse one page of the publisher listing. `*next_page_token` receives the token of the
 * following page (owned) or NULL at the last page. */
int vertex_parse_publisher_models(const char *body, struct model_info **models, size_t *n_models,
                                  char **next_page_token, char **error);

#endif /* HAX_PROVIDERS_VERTEX_MODELS_H */
