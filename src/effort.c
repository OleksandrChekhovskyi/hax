/* SPDX-License-Identifier: MIT */
#include "effort.h"

#include <stdio.h>
#include <string.h>

int effort_set_add(struct effort_set *s, const char *level)
{
    s->known = 1;
    if (!level || !*level || strlen(level) >= EFFORT_MAX_LEN)
        return 0;
    if (effort_set_has(s, level))
        return 0;
    if (s->n >= EFFORT_MAX_LEVELS)
        return 0;
    snprintf(s->v[s->n], EFFORT_MAX_LEN, "%s", level);
    s->n++;
    return 1;
}

int effort_set_has(const struct effort_set *s, const char *level)
{
    if (!s->known || !level)
        return 0;
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->v[i], level) == 0)
            return 1;
    return 0;
}
