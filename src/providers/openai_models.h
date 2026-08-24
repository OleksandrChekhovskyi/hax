/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_OPENAI_MODELS_H
#define HAX_PROVIDERS_OPENAI_MODELS_H

#include "provider.h"

/* The OpenAI-style metadata dialect: the flat {"data": [...]} /models listing served by both
 * OpenAI wires and most compatible endpoints. Installed by http_provider on providers whose
 * metadata_api resolves to the OpenAI side; operates on http_provider instances, refining each
 * entry through the preset's parse_model hook. */

int openai_list_models(struct provider *provider, struct model_info **models, size_t *n_models,
                       char **error, http_tick_cb tick, void *tick_user);

#endif /* HAX_PROVIDERS_OPENAI_MODELS_H */
