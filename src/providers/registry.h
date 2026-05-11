/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_REGISTRY_H
#define HAX_PROVIDERS_REGISTRY_H

#include <stdio.h>

#include "provider.h"

/*
 * The closed set of provider adapters this binary knows about. Kept here
 * (not in provider.h) so the seam stays a pure interface — provider.h
 * describes *what* a provider is; this header lists *which* providers
 * exist. Callers that need to resolve a name to a factory (startup
 * selection in main, a future /provider slash command, etc.) include
 * this header; the seam header is unaffected.
 *
 * Adding a new provider:
 *   1. Drop the implementation under src/providers/.
 *   2. Add its source to meson.build.
 *   3. Add the `extern PROVIDER_*` declaration below and append the
 *      symbol to PROVIDERS[] in registry.c, both in alphabetical order
 *      so error messages and merges stay predictable.
 */

extern const struct provider_factory PROVIDER_CODEX;
extern const struct provider_factory PROVIDER_LLAMACPP;
extern const struct provider_factory PROVIDER_MOCK;
extern const struct provider_factory PROVIDER_OPENAI;
extern const struct provider_factory PROVIDER_OPENAI_COMPAT;
extern const struct provider_factory PROVIDER_OPENROUTER;

/* Name used when no explicit selection is made (e.g. HAX_PROVIDER unset
 * at startup). Matches one entry's `name` field in the registry. */
extern const char PROVIDER_DEFAULT_NAME[];

/* Look up a factory by its `name` field. Returns NULL if no provider
 * with that name is registered. */
const struct provider_factory *provider_find(const char *name);

/* Write the space-separated list of registered provider names to `out`,
 * in registration (alphabetical) order. Used for error messages and as
 * the listing primitive for a future /provider command. */
void provider_list_names(FILE *out);

#endif /* HAX_PROVIDERS_REGISTRY_H */
