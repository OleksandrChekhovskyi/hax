/* SPDX-License-Identifier: MIT */
#ifndef HAX_MODEL_SORT_H
#define HAX_MODEL_SORT_H

/* Version-aware total order over model ids, for pickers and listings. Families group
 * alphabetically (case-insensitive); within a family newer versions come first, a bare id
 * precedes its dated snapshots (newest first), and snapshots precede named variants:
 * "gpt-5.6" < "gpt-5" < "gpt-5-2025-08-07" < "gpt-5-mini". The "x.y" and "x-y" version
 * schemas order identically. Returns a qsort-style negative/zero/positive. */
int model_id_order(const char *a, const char *b);

#endif /* HAX_MODEL_SORT_H */
