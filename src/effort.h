/* SPDX-License-Identifier: MIT */
#ifndef HAX_EFFORT_H
#define HAX_EFFORT_H

#include <stddef.h>

/*
 * The categorical reasoning-effort levels one model accepts, as the wire
 * values a request would carry ("low", "xhigh", …).
 *
 * Its own header because both sources of the answer carry one: a backend's
 * report (struct model_info, provider.h) and the models.dev snapshot
 * (struct catalog_entry, catalog.h). model_meta.h resolves between them.
 *
 * Fixed-size and allocation-free so catalog_entry stays a plain value type
 * its callers copy without a free function. The bounds clear every ladder
 * seen in the wild (six levels, longest "minimal"); overflowing either
 * drops the excess, which shortens a menu rather than corrupting it.
 *
 * `known` separates "this model takes no categorical effort" (known, n == 0
 * — a budget-mode or non-reasoning model) from "nobody said" (the zeroed
 * state, where the next source down gets its turn), the same way
 * PROVIDER_CAP_UNKNOWN does for the neighboring capability fields.
 */
#define EFFORT_MAX_LEVELS 10
#define EFFORT_MAX_LEN    16

struct effort_set {
    char v[EFFORT_MAX_LEVELS][EFFORT_MAX_LEN];
    size_t n;
    int known;
};

/* Append `level` unless it is already present, empty, or too long. Marks
 * the set known either way: a caller adding levels is answering the
 * question, even if every candidate gets rejected. Returns 1 when the
 * level was stored. */
int effort_set_add(struct effort_set *s, const char *level);

/* Does the set list `level`? Always 0 for an unknown set. */
int effort_set_has(const struct effort_set *s, const char *level);

#endif /* HAX_EFFORT_H */
