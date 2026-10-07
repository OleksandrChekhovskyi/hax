/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_COMPLETION_H
#define HAX_TEXT_COMPLETION_H

#include <stddef.h>

/* The words a word being completed may become, owned and in insertion order. Zero-initialize. */
struct completion {
    char **candidates;
    size_t count;
    size_t capacity;
};

void completion_add(struct completion *completion, const char *candidate);

/* Order the candidates bytewise. */
void completion_sort(struct completion *completion);

/* Drop the candidates that do not start with `prefix`, keeping the order of the rest. */
void completion_keep_prefixed(struct completion *completion, const char *prefix);

/* Extend `word`, a prefix of every candidate, like a shell: return the only candidate followed by
 * a space, or the prefix all candidates share when it is longer than `word`, ending on a whole
 * UTF-8 character. Return NULL when there is nothing to add. The result is malloc'd. */
char *completion_extend(const struct completion *completion, const char *word);

void completion_free(struct completion *completion);

#endif /* HAX_TEXT_COMPLETION_H */
