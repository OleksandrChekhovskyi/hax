/* SPDX-License-Identifier: MIT */
#include "provider.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

void provider_availability_clear(struct provider_availability *a)
{
    if (!a)
        return;
    free(a->url);
    if (a->headers) {
        for (char **h = a->headers; *h; h++)
            free(*h);
        free(a->headers);
    }
    memset(a, 0, sizeof(*a));
}

void model_info_init(struct model_info *m)
{
    memset(m, 0, sizeof(*m));
    m->cost_input = -1;
    m->cost_cache_read = -1;
    m->cost_output = -1;
    m->cost_cache_write = -1;
    m->cost_cache_write_1h = -1;
}

void model_info_copy(struct model_info *dst, const struct model_info *src)
{
    *dst = *src;
    dst->id = src->id ? xstrdup(src->id) : NULL;
    dst->desc = src->desc ? xstrdup(src->desc) : NULL;
}

void model_info_clear(struct model_info *m)
{
    free(m->id);
    free(m->desc);
    memset(m, 0, sizeof(*m));
}

void model_info_free(struct model_info *models, size_t n)
{
    if (!models)
        return;
    for (size_t i = 0; i < n; i++)
        model_info_clear(&models[i]);
    free(models);
}
