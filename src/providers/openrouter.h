/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_OPENROUTER_H
#define HAX_PROVIDERS_OPENROUTER_H

#include <jansson.h>

#include "provider.h"

/* Parse one OpenRouter /models entry into initialized `info`. Newly allocated fields are owned by
 * `info`; unreported fields retain their unknown values. */
void openrouter_parse_model(const json_t *entry, struct model_info *info);

/* Parse categorical reasoning levels from one OpenRouter /models entry. An absent level list leaves
 * `efforts` unknown unless the entry explicitly excludes reasoning parameters. */
void openrouter_parse_efforts(const json_t *entry, struct effort_set *efforts);

/* Parse the exact `model` from a filtered /models response into initialized `info`. */
void openrouter_parse_model_probe_response(const char *body, const char *model,
                                           struct model_info *info);

/* Prepare an owned, filtered metadata request. `provider` is unused. */
int openrouter_probe_model(struct provider *provider, const char *model, struct model_probe *probe);

/* /usage backend: API-key spend and account credits. */
int openrouter_query_usage(struct provider *provider);

#endif /* HAX_PROVIDERS_OPENROUTER_H */
