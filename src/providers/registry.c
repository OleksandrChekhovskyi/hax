/* SPDX-License-Identifier: MIT */
#include "providers/registry.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "provider.h"
#include "util.h"
#include "providers/config_provider.h"

/* User-facing factories precede internal ones and remain in autoselect priority order. */
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
    size_t n;
    const struct provider_factory *const *cfg = config_providers(&n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(name, cfg[i]->name) == 0)
            return cfg[i];
    return NULL;
}

void provider_list_names(FILE *out)
{
    size_t factory_count;
    const struct provider_factory *const *factories = provider_all(&factory_count);
    for (size_t i = 0; i < factory_count; i++)
        fprintf(out, "%s%s", i ? " " : "", factories[i]->name);
}

const struct provider_factory *const *provider_all(size_t *out_count)
{
    /* Config-defined factories are immutable after startup, so build the merged view once. */
    static const struct provider_factory **factories;
    static size_t factory_count;
    static int initialized;
    if (!initialized) {
        size_t config_count;
        const struct provider_factory *const *config_factories = config_providers(&config_count);
        factories = xcalloc(N_BUILTINS + config_count, sizeof(*factories));
        for (size_t i = 0; i < N_BUILTINS; i++)
            if (!BUILTINS[i]->internal)
                factories[factory_count++] = BUILTINS[i];
        for (size_t i = 0; i < config_count; i++)
            if (!is_builtin(config_factories[i]->name))
                factories[factory_count++] = config_factories[i];
        initialized = 1;
    }
    *out_count = factory_count;
    return factories;
}

const struct provider_factory *provider_default(void)
{
    size_t factory_count;
    const struct provider_factory *const *factories = provider_all(&factory_count);
    return factory_count ? factories[0] : NULL;
}
