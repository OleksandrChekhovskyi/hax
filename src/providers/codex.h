/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CODEX_H
#define HAX_PROVIDERS_CODEX_H

#include <jansson.h>

#include "provider.h"

struct provider_def; /* providers/registry.h */

/* Hooks behind the codex def: a ChatGPT-subscription endpoint speaking OpenAI Responses with
 * rotating OAuth credentials (codex_auth.h), its own model-catalog shape, and a rate-limit
 * usage report. */

/* Available iff usable credentials exist on disk. */
void codex_prepare_availability(const struct provider_def *def, struct provider_availability *out);

/* Model catalog and metadata probe against the codex /models shape. */
int codex_list_models(struct provider *provider, struct model_info **models, size_t *n_models,
                      char **error, http_tick_cb tick, void *tick_user);
int codex_probe_model(struct provider *provider, const char *model, struct model_probe *probe);

/* Print the plan's rate-limit windows from the usage endpoint. */
int codex_query_usage(struct provider *provider);

/* Return an allocated diagnostic for a failed model-catalog request. A zero status means that
 * no HTTP response was received; a 401 is worded by the credential source, not here. */
char *codex_model_catalog_error(long http_status);

/* Re-resolve a live codex provider's credentials after /login or /logout
 * (codex_auth_source_reload on the provider's source). */
void codex_provider_reload_auth(struct provider *provider);

/* Read one catalog entry into an initialized model_info. Newly reported pointer fields are owned by
 * the model. */
void codex_parse_model(const json_t *entry, struct model_info *model);

int codex_model_is_hidden(const json_t *entry);

/* Read the wire-compatible reasoning levels reported by one Codex catalog entry. */
void codex_parse_model_efforts(const json_t *entry, struct effort_set *efforts);

#endif /* HAX_PROVIDERS_CODEX_H */
