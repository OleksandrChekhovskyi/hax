/* SPDX-License-Identifier: MIT */
#include "tools/bash_export.h"

#include <stdlib.h>

#include "util.h"

static char *g_env[4];
static size_t g_n;

void bash_export_selection(const char *provider, const char *model, const char *effort)
{
    for (size_t i = 0; i < g_n; i++)
        free(g_env[i]);
    g_n = 0;
    if (!provider || !*provider)
        return; /* no live selection — leave the parent env untouched */
    g_env[g_n++] = xasprintf("HAX_PROVIDER=%s", provider);
    g_env[g_n++] = xasprintf("HAX_MODEL=%s", model ? model : "");
    g_env[g_n++] = xasprintf("HAX_REASONING_EFFORT=%s", effort ? effort : "");
    g_env[g_n++] = xstrdup("HAX_PRESET=");
}

size_t bash_export_env(const char *const **out)
{
    *out = (const char *const *)g_env;
    return g_n;
}
