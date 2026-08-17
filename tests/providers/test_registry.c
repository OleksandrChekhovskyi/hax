/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "provider.h"
#include "providers/registry.h"

/* Position of `name` in provider_all() (the autoselect-priority order), or -1
 * when it isn't a selectable provider. */
static int idx_of(const char *name)
{
    size_t n;
    const struct provider_factory *const *all = provider_all(&n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(all[i]->id, name) == 0)
            return (int)i;
    return -1;
}

/* The default is the first (highest-priority) selectable provider. */
static void test_default_is_highest_priority(void)
{
    size_t n;
    const struct provider_factory *const *all = provider_all(&n);
    EXPECT(n > 0);
    EXPECT(provider_default() == all[0]);
    EXPECT_STR_EQ(provider_default()->id, "codex");
}

/* mock is internal: excluded from the selectable set, but still resolvable
 * by name (HAX_PROVIDER=mock keeps working). */
static void test_internal_providers_hidden(void)
{
    EXPECT(idx_of("mock") == -1);
    EXPECT(provider_find("mock") != NULL);
}

/* Autoselect-priority ordering: the compiled-in factories come first, in
 * BUILTINS order. Config-defined providers — the shipped -compatible and
 * ollama recipes, then custom blocks — are appended after every built-in,
 * so they never outrank one at cold-start autoselect. */
static void test_autoselect_order(void)
{
    int llama = idx_of("llamacpp");
    int compat = idx_of("openai-compatible");
    int ollama = idx_of("ollama");
    EXPECT(llama >= 0 && compat >= 0 && ollama >= 0);
    EXPECT(llama < compat);  /* built-in before config-defined */
    EXPECT(compat < ollama); /* recipes keep their shipped order */
}

/* The former llamacpp id keeps resolving for saved sessions and scripts. */
static void test_former_id_canonicalized(void)
{
    EXPECT(provider_find("llama.cpp") == provider_find("llamacpp"));
    EXPECT(provider_find("llamacpp") != NULL);
}

/* Picker labels resolve without construction: a configured display_name wins, then the
 * factory default, then the id. Each display-name variable renames only its own provider. */
static void test_display_name_resolution(void)
{
    unsetenv("HAX_OPENAI_DISPLAY_NAME");
    EXPECT_STR_EQ(provider_display_name(provider_find("llamacpp")), "llama.cpp");
    EXPECT_STR_EQ(provider_display_name(provider_find("openai")), "openai");
    EXPECT_STR_EQ(provider_display_name(provider_find("openai-compatible")), "openai-compatible");

    setenv("HAX_OPENAI_DISPLAY_NAME", "vLLM", 1);
    EXPECT_STR_EQ(provider_display_name(provider_find("openai-compatible")), "vLLM");
    EXPECT_STR_EQ(provider_display_name(provider_find("anthropic-compatible")),
                  "anthropic-compatible");
    unsetenv("HAX_OPENAI_DISPLAY_NAME");
}

int main(void)
{
    test_default_is_highest_priority();
    test_internal_providers_hidden();
    test_autoselect_order();
    test_former_id_canonicalized();
    test_display_name_resolution();
    T_REPORT();
}
