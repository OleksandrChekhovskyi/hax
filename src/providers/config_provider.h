/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CONFIG_PROVIDER_H
#define HAX_PROVIDERS_CONFIG_PROVIDER_H

#include <stddef.h>

#include "provider.h"

/* Providers defined by data rather than code.
 *
 * A config-defined provider is a named `providers.<name>` block in config.json, optionally
 * seeded by a built-in recipe of defaults (see RECIPES in config_provider.c — e.g. "ollama").
 * Each speaks one dialect — "openai-completions" or "anthropic-messages" — and resolves its
 * settings from its own subtree overlaid on the recipe. The API key is the one value read
 * from the environment, via a recipe- or config-declared api_key_env: a secret belongs in
 * the environment, not the config file. */

/* The dynamic factory set: the union of recipe names and config.json `providers.*` names,
 * deduplicated — a config block matching a recipe name overlays that recipe rather than
 * adding a second entry. Heap-built once on first call and cached for the process; *n
 * receives the count. Names are not filtered against the compiled-in factories; the
 * registry does that when merging. */
const struct provider_factory *const *config_providers(size_t *n);

#endif /* HAX_PROVIDERS_CONFIG_PROVIDER_H */
