/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_USAGE_FETCH_H
#define HAX_PROVIDERS_USAGE_FETCH_H

#include <jansson.h>

/* GET a provider's JSON /usage document under a cancellable busy indicator. Failures are reported
 * through ui_error — a 401 as `provider_label` rejecting the configured API key, others naming
 * `url` — while a cancelled fetch reports nothing. Returns the owned root, or NULL. */
json_t *usage_fetch_json(const char *url, const char *const *headers, const char *provider_label);

#endif /* HAX_PROVIDERS_USAGE_FETCH_H */
