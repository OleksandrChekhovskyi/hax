/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "harness.h"
#include "providers/registry.h"

/* Position of `name` in provider_all() (the autoselect-priority order), or -1
 * when it isn't a selectable provider. */
static int idx_of(const char *name)
{
    size_t n;
    const struct provider_factory *const *all = provider_all(&n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(all[i]->name, name) == 0)
            return (int)i;
    return -1;
}

int main(void)
{
    size_t n;
    const struct provider_factory *const *all = provider_all(&n);
    EXPECT(n > 0);

    /* The default is the first (highest-priority) selectable provider. */
    EXPECT(provider_default() == all[0]);
    EXPECT_STR_EQ(provider_default()->name, "codex");

    /* mock is internal: excluded from the selectable set, but still resolvable
     * by name (HAX_PROVIDER=mock keeps working). */
    EXPECT(idx_of("mock") == -1);
    EXPECT(provider_find("mock") != NULL);

    /* Autoselect-priority ordering: the compiled-in factories come first, in
     * BUILTINS order, so the generic openai-compatible preset ranks ahead of
     * the local llama.cpp server. Config-defined providers (ollama is a
     * shipped recipe) are appended after every built-in, so they never
     * outrank one at cold-start autoselect. Guard both halves. */
    int compat = idx_of("openai-compatible");
    int llama = idx_of("llama.cpp");
    int ollama = idx_of("ollama");
    EXPECT(compat >= 0 && llama >= 0 && ollama >= 0);
    EXPECT(compat < llama);
    EXPECT(llama < ollama); /* built-in before config-defined */

    T_REPORT();
}
