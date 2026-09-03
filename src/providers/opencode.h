/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_OPENCODE_H
#define HAX_PROVIDERS_OPENCODE_H

#include <jansson.h>
#include <stddef.h>

#include "providers/usage_render.h"

struct provider;

/* OpenCode gateway extras beyond the shared defs: the Go subscription reports rate-limit
 * windows on <base_url>/usage; Zen exposes no balance or usage API yet. */

/* Fill up to `max` windows from a Go /usage response, one per well-formed entry of the `usage`
 * object, in response order. Labels and notes borrow from `root`. Returns the count. */
size_t opencode_usage_parse(json_t *root, struct usage_window *windows, size_t max);

/* Owned NULL-terminated headers for a usage request: Bearer auth independent of the model wire,
 * plus the provider's extra headers. The provider must hold a resolved API key. Free with
 * string_array_free. */
char **opencode_usage_headers(const struct provider *provider);

/* /usage backend for the opencode-go def. */
int opencode_go_query_usage(struct provider *provider);

#endif /* HAX_PROVIDERS_OPENCODE_H */
