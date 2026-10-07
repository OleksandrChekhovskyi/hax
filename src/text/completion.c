/* SPDX-License-Identifier: MIT */
#include "text/completion.h"

#include <stdlib.h>
#include <string.h>

#include "xalloc.h"

void completion_add(struct completion *completion, const char *candidate)
{
    if (completion->count == completion->capacity) {
        completion->capacity = completion->capacity ? completion->capacity * 2 : 16;
        completion->candidates = xrealloc(completion->candidates,
                                          completion->capacity * sizeof(*completion->candidates));
    }
    completion->candidates[completion->count++] = xstrdup(candidate);
}

static int compare_candidates(const void *left, const void *right)
{
    return strcmp(*(char *const *)left, *(char *const *)right);
}

void completion_sort(struct completion *completion)
{
    /* An empty completion's array is NULL, which qsort must not receive even with a zero count. */
    if (completion->count > 1)
        qsort(completion->candidates, completion->count, sizeof(*completion->candidates),
              compare_candidates);
}

void completion_keep_prefixed(struct completion *completion, const char *prefix)
{
    size_t prefix_len = strlen(prefix);
    size_t kept = 0;

    for (size_t i = 0; i < completion->count; i++) {
        if (strncmp(completion->candidates[i], prefix, prefix_len) == 0)
            completion->candidates[kept++] = completion->candidates[i];
        else
            free(completion->candidates[i]);
    }
    completion->count = kept;
}

static size_t shared_prefix_len(const struct completion *completion)
{
    const char *first = completion->candidates[0];
    size_t shared = strlen(first);

    for (size_t i = 1; i < completion->count; i++) {
        size_t common = 0;
        while (common < shared && completion->candidates[i][common] == first[common])
            common++;
        shared = common;
    }
    /* Candidates that differ inside a multibyte character share only its leading bytes; stop
     * before that character rather than emit a partial one. */
    while (shared > 0 && ((unsigned char)first[shared] & 0xc0) == 0x80)
        shared--;
    return shared;
}

char *completion_extend(const struct completion *completion, const char *word)
{
    if (completion->count == 0)
        return NULL;
    if (completion->count == 1)
        return xasprintf("%s ", completion->candidates[0]);

    size_t shared = shared_prefix_len(completion);
    if (shared <= strlen(word))
        return NULL;
    return xasprintf("%.*s", (int)shared, completion->candidates[0]);
}

void completion_free(struct completion *completion)
{
    for (size_t i = 0; i < completion->count; i++)
        free(completion->candidates[i]);
    free(completion->candidates);
    completion->candidates = NULL;
    completion->count = 0;
    completion->capacity = 0;
}
