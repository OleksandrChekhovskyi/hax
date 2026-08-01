/* SPDX-License-Identifier: MIT */
#include "effort.h"

#include <stdio.h>
#include <string.h>

static const char *const EFFORT_ORDER[] = {"none", "minimal", "low", "medium",
                                           "high", "xhigh",   "max"};

static int effort_rank(const char *level)
{
    if (!level)
        return -1;
    for (size_t i = 0; i < sizeof(EFFORT_ORDER) / sizeof(EFFORT_ORDER[0]); i++)
        if (strcmp(EFFORT_ORDER[i], level) == 0)
            return (int)i;
    return -1;
}

int effort_set_add(struct effort_set *set, const char *level)
{
    set->known = 1;
    if (!level || !*level || strlen(level) >= EFFORT_MAX_LEN)
        return 0;
    if (effort_set_has(set, level))
        return 0;
    if (set->count >= EFFORT_MAX_LEVELS)
        return 0;
    snprintf(set->values[set->count], EFFORT_MAX_LEN, "%s", level);
    set->count++;
    return 1;
}

int effort_set_has(const struct effort_set *set, const char *level)
{
    if (!set->known || !level)
        return 0;
    for (size_t i = 0; i < set->count; i++)
        if (strcmp(set->values[i], level) == 0)
            return 1;
    return 0;
}

const char *effort_clamp(const struct effort_set *set, const char *requested)
{
    if (!set->known || set->count == 0 || !requested || !*requested)
        return NULL;
    if (effort_set_has(set, requested))
        return requested;

    int requested_rank = effort_rank(requested);
    if (requested_rank < 0)
        return NULL;

    const char *lower = NULL;
    const char *upper = NULL;
    int lower_rank = -1;
    int upper_rank = 0;
    for (size_t i = 0; i < set->count; i++) {
        int rank = effort_rank(set->values[i]);
        if (rank < 0)
            continue;
        if (rank <= requested_rank && rank > lower_rank) {
            lower = set->values[i];
            lower_rank = rank;
        }
        if (rank > requested_rank && (!upper || rank < upper_rank)) {
            upper = set->values[i];
            upper_rank = rank;
        }
    }
    return lower ? lower : upper;
}
