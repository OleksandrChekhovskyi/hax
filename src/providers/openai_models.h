/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_OPENAI_MODELS_H
#define HAX_PROVIDERS_OPENAI_MODELS_H

#include "provider.h"

/* The OpenAI-style metadata dialect: the flat {"data": [...]} /models listing served by both
 * OpenAI wires and most compatible endpoints. Installed by http_provider on providers whose
 * metadata_api resolves to the OpenAI side; operates on http_provider instances, refining each
 * entry through the def's parse_model hook. */

int openai_list_models(struct provider *provider, struct model_info **models, size_t *n_models,
                       char **error, http_tick_cb tick, void *tick_user);

/* Probe one model through the full listing, refined by the def's parse_model hook; installed only
 * for defs with one, since a bare id list says nothing about the model. */
int openai_probe_model(struct provider *provider, const char *model, struct model_probe *probe);

#endif /* HAX_PROVIDERS_OPENAI_MODELS_H */
