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
 *   3. Add the `extern PROVIDER_*` declaration below (alphabetical, for an
 *      easy diff) and insert the symbol into PROVIDERS[] in registry.c at the
 *      right autoselect-priority position (see the array comment there).
 */

extern const struct provider_factory PROVIDER_CODEX;
extern const struct provider_factory PROVIDER_LLAMACPP;
extern const struct provider_factory PROVIDER_MOCK;
extern const struct provider_factory PROVIDER_OPENAI;
extern const struct provider_factory PROVIDER_OPENAI_COMPAT;
extern const struct provider_factory PROVIDER_OPENROUTER;

/*
 * Beyond these compiled-in factories, the registry also surfaces
 * config-defined providers — named providers.<name> blocks in config.json,
 * plus the shipped recipes (see providers/config_provider.c). They are
 * merged in below the compiled-in set: a built-in name always wins, and a
 * config/recipe provider adds a new selectable name. So provider_find /
 * provider_all / the /provider picker see ollama (a recipe) and any custom
 * provider the user configured, without a code change.
 */

/* Look up a factory by its `name` field. Returns NULL if no provider
 * with that name is registered. */
const struct provider_factory *provider_find(const char *name);

/* The highest-priority user-facing provider — the default used when nothing
 * is configured (HAX_PROVIDER unset, no state.json pick): provider_all()'s
 * first entry. Cold-start autoselect tries it first; the one-shot path builds
 * it directly. */
const struct provider_factory *provider_default(void);

/* The full registry as a read-only array; *n receives the count. In
 * autoselect-priority order (most-preferred first), internal providers
 * excluded. The listing primitive the /provider picker iterates to show —
 * and probe the availability of — every selectable provider. */
const struct provider_factory *const *provider_all(size_t *n);

/* Write the space-separated list of selectable provider names to `out`, in
 * priority order. Used for error messages and the supported-list. */
void provider_list_names(FILE *out);

#endif /* HAX_PROVIDERS_REGISTRY_H */
