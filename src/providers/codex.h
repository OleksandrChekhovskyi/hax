/* SPDX-License-Identifier: MIT */
#ifndef HAX_CODEX_H
#define HAX_CODEX_H

#include <jansson.h>

#include "provider.h"

/* Create a provider from ~/.codex/auth.json, or report an error and return NULL. */
struct provider *codex_provider_new(const char *name);

/* Return an allocated diagnostic for a failed model-catalog request. A zero status means that no
 * HTTP response was received. */
char *codex_model_catalog_error(long http_status);

/* Read one catalog entry into an initialized model_info. Newly reported pointer fields are owned by
 * the model. */
void codex_parse_model(const json_t *entry, struct model_info *model);

int codex_model_is_hidden(const json_t *entry);

/* Read the wire-compatible reasoning levels reported by one Codex catalog entry. */
void codex_parse_model_efforts(const json_t *entry, struct effort_set *efforts);

/* Return a new Responses API input array for the flat conversation items. Opaque reasoning is
 * included only when its provider and model match the request. */
json_t *codex_build_input_items(const struct item *items, size_t n_items, const char *provider,
                                const char *model, int image_input);

extern const struct provider_factory PROVIDER_CODEX;

#endif /* HAX_CODEX_H */
