/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <string.h>

#include "providers/config_provider.h"
#include "providers/registry.h"
#include "util.h"

/* Compiled-in factories, in autoselect-priority order (most-preferred first)
 * — also the order the /provider picker and the "supported" list read in,
 * ahead of any config-defined providers, which are appended after these. The
 * rationale, top to bottom: the built-in default (codex); the configured
 * generic OpenAI-compatible endpoint (selectable only when
 * HAX_OPENAI_BASE_URL is set, so it naturally wins that case); the local
 * llama.cpp server on its own port; then cloud-key backends (openai,
 * openrouter). provider_all()[0] is the default. mock is internal (filtered
 * out of provider_all) and sits last. One factory per line so an addition is
 * a single-line diff. */
// clang-format off
static const struct provider_factory *const BUILTINS[] = {
    &PROVIDER_CODEX,
    &PROVIDER_OPENAI_COMPAT,
    &PROVIDER_ANTHROPIC_COMPAT,
    &PROVIDER_LLAMACPP,
    &PROVIDER_OPENAI,
    &PROVIDER_ANTHROPIC,
    &PROVIDER_OPENROUTER,
    &PROVIDER_MOCK,
};
// clang-format on
#define N_BUILTINS (sizeof(BUILTINS) / sizeof(BUILTINS[0]))

/* True when `name` is a compiled-in factory — used to give the built-ins
 * precedence over a config block of the same name when merging. */
static int is_builtin(const char *name)
{
    for (size_t i = 0; i < N_BUILTINS; i++)
        if (strcmp(name, BUILTINS[i]->name) == 0)
            return 1;
    return 0;
}

const struct provider_factory *provider_find(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < N_BUILTINS; i++)
        if (strcmp(name, BUILTINS[i]->name) == 0)
            return BUILTINS[i];
    /* Fall through to config-defined providers (recipes + config.json
     * providers.*). A built-in of the same name was matched above, so it
     * always wins. */
    size_t n;
    const struct provider_factory *const *cfg = config_providers(&n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(name, cfg[i]->name) == 0)
            return cfg[i];
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
    /* The user-facing provider set: the non-internal compiled-in factories,
     * then config-defined providers that don't shadow a built-in name. Both
     * the /provider picker and cold-start auto-selection enumerate this, so a
     * backend like mock is never offered or auto-picked — yet provider_find
     * still resolves it by name, keeping HAX_PROVIDER=mock working. Built once
     * (config-defined names are fixed for the run, loaded from config.json at
     * startup); provider_all runs only on the single-threaded foreground path
     * (startup auto-select, the interactive picker). */
    static const struct provider_factory **list;
    static size_t count;
    static int built;
    if (!built) {
        size_t n_cfg;
        const struct provider_factory *const *cfg = config_providers(&n_cfg);
        list = xcalloc(N_BUILTINS + n_cfg, sizeof(*list));
        for (size_t i = 0; i < N_BUILTINS; i++)
            if (!BUILTINS[i]->internal)
                list[count++] = BUILTINS[i];
        for (size_t i = 0; i < n_cfg; i++)
            if (!is_builtin(cfg[i]->name))
                list[count++] = cfg[i];
        built = 1;
    }
    *n = count;
    return list;
}

const struct provider_factory *provider_default(void)
{
    size_t n;
    const struct provider_factory *const *all = provider_all(&n);
    return n ? all[0] : NULL; /* first = highest autoselect priority */
}
