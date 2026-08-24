/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_ANTHROPIC_MODELS_H
#define HAX_PROVIDERS_ANTHROPIC_MODELS_H

#include <jansson.h>

#include "provider.h"

/* The Anthropic-style metadata dialect: the cursor-paginated /models listing, the model
 * probe, and per-entry capability parsing. Installed by http_provider on providers whose
 * metadata_api resolves to the Anthropic side; operates on http_provider instances. */

int anthropic_list_models(struct provider *provider, struct model_info **models, size_t *n_models,
                          char **error, http_tick_cb tick, void *tick_user);

int anthropic_probe_model(struct provider *provider, const char *model, struct model_probe *probe);

/* `out` is initialized and already owns the entry's id. */
void anthropic_parse_model(const json_t *entry, struct model_info *out);

#endif /* HAX_PROVIDERS_ANTHROPIC_MODELS_H */
