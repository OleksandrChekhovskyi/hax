/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CODEX_H
#define HAX_PROVIDERS_CODEX_H

#include <jansson.h>

#include "provider.h"

struct provider_def; /* providers/registry.h */

/* Create a provider from ~/.codex/auth.json, or report an error and return NULL. */
struct provider *codex_provider_new(const struct provider_def *def);

/* Available iff usable credentials exist on disk. */
void codex_prepare_availability(const struct provider_def *def, struct provider_availability *out);

/* Return an allocated diagnostic for a failed model-catalog request. A zero status means that no
 * HTTP response was received; a 401 reports `token_expired`, whose wording depends on the
 * credential source. */
char *codex_model_catalog_error(long http_status, const char *token_expired);

/* Re-resolve a live codex provider's credentials after /login or /logout. When none remain the
 * auth is cleared and subsequent requests report "not logged in" rather than reusing the removed
 * token. */
void codex_provider_reload_auth(struct provider *provider);

/* Read one catalog entry into an initialized model_info. Newly reported pointer fields are owned by
 * the model. */
void codex_parse_model(const json_t *entry, struct model_info *model);

int codex_model_is_hidden(const json_t *entry);

/* Read the wire-compatible reasoning levels reported by one Codex catalog entry. */
void codex_parse_model_efforts(const json_t *entry, struct effort_set *efforts);

#endif /* HAX_PROVIDERS_CODEX_H */
