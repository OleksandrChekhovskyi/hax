/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdlib.h>

#include "harness.h"
#include "xalloc.h"

static void test_string_array_concat(void)
{
    EXPECT(string_array_concat(NULL, NULL) == NULL);

    /* A leading NULL entry terminates the array: {NULL} counts as empty. */
    const char *empty[] = {NULL};
    EXPECT(string_array_concat(empty, NULL) == NULL);

    const char *first[] = {"a: 1", NULL};
    const char *second[] = {"b: 2", "c: 3", NULL};
    char **combined = string_array_concat(first, second);
    EXPECT(combined != NULL);
    if (combined) {
        EXPECT_STR_EQ(combined[0], "a: 1");
        EXPECT_STR_EQ(combined[1], "b: 2");
        EXPECT_STR_EQ(combined[2], "c: 3");
        EXPECT(combined[3] == NULL);
        string_array_free(combined);
    }

    char **only_second = string_array_concat(NULL, second);
    EXPECT(only_second != NULL);
    if (only_second) {
        EXPECT_STR_EQ(only_second[0], "b: 2");
        EXPECT_STR_EQ(only_second[1], "c: 3");
        EXPECT(only_second[2] == NULL);
        string_array_free(only_second);
    }
}

static void test_zero_sized_allocations(void)
{
    void *malloc_result = xmalloc(0);
    void *calloc_result = xcalloc(0, SIZE_MAX);
    void *realloc_result = xrealloc(NULL, 0);

    EXPECT(malloc_result != NULL);
    EXPECT(calloc_result != NULL);
    EXPECT(realloc_result != NULL);
    free(malloc_result);
    free(calloc_result);
    free(realloc_result);
}

int main(void)
{
    test_string_array_concat();
    test_zero_sized_allocations();

    T_REPORT();
}
