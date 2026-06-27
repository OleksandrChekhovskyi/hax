/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CONFIG_PROVIDER_H
#define HAX_PROVIDERS_CONFIG_PROVIDER_H

#include <stddef.h>

#include "provider.h"

/*
 * Providers defined by data rather than code.
 *
 * A config-defined provider is a named `providers.<name>` block in
 * config.json (the file/runtime-override lane — there is no per-provider env
 * binding; the env/ad-hoc lane is the global "openai-compatible" preset),
 * optionally seeded by a built-in *recipe* (see RECIPES in
 * config_provider.c — e.g. "ollama"). Each speaks one dialect — either
 * "openai-completions" (openai_provider_new_preset) or "anthropic-messages"
 * (anthropic_provider_new_preset) — and resolves its settings from its own
 * subtree overlaid on its recipe defaults. The API key is the one value read
 * from the environment, via a
 * recipe- or config-declared key var (api_key_env), since a secret belongs
 * in the environment, not the config file.
 *
 * The registry (registry.c) merges these dynamic factories with the
 * compiled-in ones: a built-in factory name wins, config/recipe providers
 * add new selectable names.
 */

/* The dynamic factory set: the union of recipe names and config.json
 * `providers.*` names (deduplicated; a config block matching a recipe name
 * overlays it rather than adding a second entry). Each is a heap-built
 * factory whose `name` is the provider id and whose new/available hooks read
 * that id's config + recipe. Built once on first call and cached for the
 * process; *n receives the count. Names are NOT filtered against the
 * compiled-in set here — the registry does that when merging. */
const struct provider_factory *const *config_providers(size_t *n);

#endif /* HAX_PROVIDERS_CONFIG_PROVIDER_H */
