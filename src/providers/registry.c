/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <string.h>

#include "providers/registry.h"

/* One factory per line so additions remain a single-line diff and the
 * alphabetical order is obvious at a glance. */
// clang-format off
static const struct provider_factory *const PROVIDERS[] = {
    &PROVIDER_CODEX,
    &PROVIDER_LLAMACPP,
    &PROVIDER_MOCK,
    &PROVIDER_OLLAMA,
    &PROVIDER_OPENAI,
    &PROVIDER_OPENAI_COMPAT,
    &PROVIDER_OPENROUTER,
};
// clang-format on
#define N_PROVIDERS (sizeof(PROVIDERS) / sizeof(PROVIDERS[0]))

const char PROVIDER_DEFAULT_NAME[] = "codex";

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
    for (size_t i = 0; i < N_PROVIDERS; i++)
        fprintf(out, "%s%s", i ? " " : "", PROVIDERS[i]->name);
}
