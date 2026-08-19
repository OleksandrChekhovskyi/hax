/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_ANTHROPIC_H
#define HAX_PROVIDERS_ANTHROPIC_H

#include <jansson.h>

#include "provider.h"
#include "providers/http_provider.h"

/* Messages-family provider construction: forces the Messages wire and effort ladder onto the
 * preset and attaches the Anthropic metadata protocol (paginated /models, model probe). */
struct provider *anthropic_provider_new_preset(const struct http_provider_preset *preset);

/* Construct the api.anthropic.com provider; `id` becomes its stable identity. */
struct provider *anthropic_provider_new(const char *id);

/* `out` is initialized and already owns the entry's id. */
void anthropic_parse_model(const json_t *entry, struct model_info *out);

extern const struct provider_factory PROVIDER_ANTHROPIC;

#endif /* HAX_PROVIDERS_ANTHROPIC_H */
