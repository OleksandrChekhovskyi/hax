/* SPDX-License-Identifier: MIT */
#ifndef HAX_EFFORT_H
#define HAX_EFFORT_H

#include <stddef.h>

/* Categorical reasoning-effort values accepted on the wire. `known` distinguishes an explicitly
 * empty set from absent metadata. The fixed bounds keep catalog entries allocation-free; excess
 * or overlong values are ignored. */
#define EFFORT_MAX_LEVELS 10
#define EFFORT_MAX_LEN    16

struct effort_set {
    char values[EFFORT_MAX_LEVELS][EFFORT_MAX_LEN];
    size_t count;
    int known;
};

/* Add `level` unless it is duplicate, empty, too long, or beyond capacity. The set becomes known
 * even when the value is rejected. Returns 1 when stored and 0 otherwise. */
int effort_set_add(struct effort_set *set, const char *level);

/* Return whether the known set contains `level`. */
int effort_set_has(const struct effort_set *set, const char *level);

/* Return `requested` when offered; otherwise return the nearest lower known level, or the nearest
 * higher level when none is lower. Returns NULL for an empty set or an unrecognized request. The
 * result is borrowed from `set`, except when returning `requested` unchanged. */
const char *effort_clamp(const struct effort_set *set, const char *requested);

#endif /* HAX_EFFORT_H */
