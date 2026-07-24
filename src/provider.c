/* SPDX-License-Identifier: MIT */
#include "provider.h"

#include <stdlib.h>
#include <string.h>

void model_info_init(struct model_info *m)
{
    memset(m, 0, sizeof(*m));
    m->cost_input = -1;
    m->cost_cache_read = -1;
    m->cost_output = -1;
}

void model_info_free(struct model_info *models, size_t n)
{
    if (!models)
        return;
    for (size_t i = 0; i < n; i++) {
        free(models[i].id);
        free(models[i].desc);
    }
    free(models);
}
