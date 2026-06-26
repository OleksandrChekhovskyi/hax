/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <string.h>

#include "providers/registry.h"

/* Autoselect-priority order, most-preferred first — also the order the
 * /provider picker and the "supported" list read in. The rationale, top to
 * bottom: the built-in default (codex); the configured generic OpenAI-
 * compatible endpoint (selectable only when HAX_OPENAI_BASE_URL is set, so it
 * naturally wins that case over the local presets, which honor the same
 * override); local servers on their own ports (llama.cpp, ollama); then
 * cloud-key backends (openai, openrouter). provider_all()[0] is the default.
 * mock is internal (filtered out of provider_all) and sits last. One factory
 * per line so an addition is a single-line diff. */
// clang-format off
static const struct provider_factory *const PROVIDERS[] = {
    &PROVIDER_CODEX,
    &PROVIDER_OPENAI_COMPAT,
    &PROVIDER_LLAMACPP,
    &PROVIDER_OLLAMA,
    &PROVIDER_OPENAI,
    &PROVIDER_OPENROUTER,
    &PROVIDER_MOCK,
};
// clang-format on
#define N_PROVIDERS (sizeof(PROVIDERS) / sizeof(PROVIDERS[0]))

const struct provider_factory *provider_find(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < N_PROVIDERS; i++) {
        if (strcmp(name, PROVIDERS[i]->name) == 0)
            return PROVIDERS[i];
    }
    return NULL;
}

void provider_list_names(FILE *out)
{
    size_t n;
    const struct provider_factory *const *f = provider_all(&n);
    for (size_t i = 0; i < n; i++)
        fprintf(out, "%s%s", i ? " " : "", f[i]->name);
}

const struct provider_factory *const *provider_all(size_t *n)
{
    /* The user-facing provider set: PROVIDERS minus internal (dev-only)
     * factories. Both the /provider picker and cold-start auto-selection
     * enumerate this, so a backend like mock is never offered or
     * auto-picked — yet provider_find still resolves it by name, keeping
     * HAX_PROVIDER=mock working. Built once; the registry is constant and
     * provider_all runs only on the single-threaded foreground path
     * (startup auto-select, the interactive picker). */
    static const struct provider_factory *selectable[N_PROVIDERS];
    static size_t n_selectable;
    static int built;
    if (!built) {
        for (size_t i = 0; i < N_PROVIDERS; i++)
            if (!PROVIDERS[i]->internal)
                selectable[n_selectable++] = PROVIDERS[i];
        built = 1;
    }
    *n = n_selectable;
    return selectable;
}

const struct provider_factory *provider_default(void)
{
    size_t n;
    const struct provider_factory *const *all = provider_all(&n);
    return n ? all[0] : NULL; /* first = highest autoselect priority */
}
