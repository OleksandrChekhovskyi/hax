/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_REGISTRY_H
#define HAX_PROVIDERS_REGISTRY_H

#include <stdio.h>

#include "provider.h"

/* Built-in provider factories. Discovery and ordering are exposed below, outside the provider
 * interface in provider.h. */

extern const struct provider_factory PROVIDER_ANTHROPIC;
extern const struct provider_factory PROVIDER_CODEX;
extern const struct provider_factory PROVIDER_LLAMACPP;
extern const struct provider_factory PROVIDER_MOCK;
extern const struct provider_factory PROVIDER_OPENAI;
extern const struct provider_factory PROVIDER_OPENROUTER;

/* Look up a built-in, recipe, or config-defined factory by name, accepting former ids. Built-in
 * names take precedence; returns NULL when no provider is registered. */
const struct provider_factory *provider_find(const char *name);

/* Display label for a factory without constructing its provider: the configured
 * providers.<name>.display_name, else the factory's default, else the id. Borrowed; valid until
 * the next config write. */
const char *provider_display_name(const struct provider_factory *factory);

/* Return the highest-priority user-facing provider, or NULL when none is registered. */
const struct provider_factory *provider_default(void);

/* Return the read-only user-facing registry in autoselect order; internal providers are excluded.
 * `count` receives its length. Foreground-thread only. */
const struct provider_factory *const *provider_all(size_t *count);

/* Write the space-separated user-facing provider names in autoselect order. */
void provider_list_names(FILE *out);

#endif /* HAX_PROVIDERS_REGISTRY_H */
