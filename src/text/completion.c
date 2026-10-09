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

/* Return the end of the part of `candidate` that continues at byte `from`: just past the next
 * separator, or the candidate's end. */
static size_t part_end(const struct completion *completion, const char *candidate, size_t from)
{
    size_t len = strlen(candidate);
    const char *separator =
        completion->separator ? memchr(candidate + from, completion->separator, len - from) : NULL;
    return separator ? (size_t)(separator + 1 - candidate) : len;
}

char *completion_extend(const struct completion *completion, const char *word)
{
    if (completion->count == 0)
        return NULL;

    const char *first = completion->candidates[0];
    size_t word_len = strlen(word);
    size_t shared = completion->count == 1 ? strlen(first) : shared_prefix_len(completion);
    size_t part = part_end(completion, first, word_len);
    if (completion->separator && part > word_len && part <= shared &&
        first[part - 1] == completion->separator)
        return xasprintf("%.*s", (int)part, first);
    if (completion->count == 1)
        return xasprintf("%s ", first);
    if (shared <= word_len)
        return NULL;
    return xasprintf("%.*s", (int)shared, first);
}

void completion_to_parts(struct completion *completion, const char *word)
{
    const char *last_separator =
        completion->separator ? strrchr(word, completion->separator) : NULL;
    size_t part_start = last_separator ? (size_t)(last_separator + 1 - word) : 0;
    size_t word_len = strlen(word);
    size_t kept = 0;

    for (size_t i = 0; i < completion->count; i++) {
        char *candidate = completion->candidates[i];
        size_t end = part_end(completion, candidate, word_len);
        char *part = xasprintf("%.*s", (int)(end - part_start), candidate + part_start);
        free(candidate);

        int duplicate = 0;
        for (size_t j = 0; j < kept && !duplicate; j++)
            duplicate = strcmp(completion->candidates[j], part) == 0;
        if (duplicate)
            free(part);
        else
            completion->candidates[kept++] = part;
    }
    completion->count = kept;
}

void completion_free(struct completion *completion)
{
    for (size_t i = 0; i < completion->count; i++)
        free(completion->candidates[i]);
    free(completion->candidates);
    completion->candidates = NULL;
    completion->count = 0;
    completion->capacity = 0;
    completion->separator = '\0';
}
